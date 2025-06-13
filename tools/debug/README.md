# Goo Debugger (goo-debug)

A full-featured debugger for Goo programs with DWARF debugging information support, providing comprehensive debugging capabilities including breakpoints, stack inspection, variable examination, and source-level debugging.

## Features

### Core Debugging Features
- **Process Control** - Start, attach, detach, and control program execution
- **Breakpoints** - Set, remove, enable/disable breakpoints at addresses, lines, or functions
- **Step Execution** - Step into, over, and out of functions with source-level granularity
- **Stack Inspection** - Complete stack trace with frame selection and local variable access
- **Variable Examination** - Inspect and modify variable values with type information
- **Memory Inspection** - Examine raw memory contents and disassemble instructions
- **Register Access** - View and modify CPU register contents
- **Source Display** - Show source code with current execution position

### DWARF Integration
- **Debug Information** - Full DWARF debugging information support
- **Source Mapping** - Map between source lines and assembly addresses
- **Type Information** - Rich type information for variables and expressions
- **Symbol Resolution** - Function and variable name resolution
- **Inline Functions** - Support for inlined function debugging

### Goo Language Support
- **Ownership Semantics** - Debug ownership transfer and borrowing
- **Error Unions** - Inspect error union states and propagation
- **Nullable Types** - Debug nullable type checking and unwrapping
- **Channel Operations** - Debug concurrent operations and channel states
- **Unsafe Blocks** - Debug low-level hardware control code

## Installation

### Prerequisites

Install required dependencies:

```bash
# Ubuntu/Debian
sudo apt-get install libdwarf-dev libelf-dev

# macOS with Homebrew
brew install libdwarf libelf

# Fedora/RHEL
sudo dnf install libdwarf-devel elfutils-libelf-devel
```

### Build from Source

```bash
cd tools/debug
make deps  # Check dependencies
make       # Build the debugger
```

### Install System-wide

```bash
make install PREFIX=/usr/local
```

## Usage

### Basic Usage

```bash
# Debug a program
goo-debug my_program

# Debug with arguments
goo-debug my_program -- arg1 arg2

# Attach to running process
goo-debug -p 1234

# Analyze core dump
goo-debug -c core.dump my_program

# Enable verbose output
goo-debug -v my_program
```

### Command Line Options

- `-p, --pid <pid>` - Attach to running process
- `-c, --core <file>` - Analyze core dump
- `-v, --verbose` - Enable verbose output
- `-h, --help` - Show help message

## Interactive Commands

### Process Control

```bash
# Start or restart program execution
run [args]

# Continue execution
continue
c

# Interrupt running program
interrupt
```

### Breakpoints

```bash
# Set breakpoint at current line
break

# Set breakpoint at specific line
break 42
break main.goo:42

# Set breakpoint at address
break 0x401234

# Set breakpoint at function
break main
break add_numbers

# List all breakpoints
info breakpoints

# Delete breakpoint
delete 1

# Enable/disable breakpoint
enable 1
disable 1

# Set conditional breakpoint
break main.goo:42 if x > 10
```

### Step Execution

```bash
# Step one instruction
step
s

# Step into function calls
stepi
si

# Step over function calls (source level)
next
n

# Step out of current function
finish

# Continue until return
until
```

### Stack Inspection

```bash
# Show stack trace
backtrace
bt

# Show detailed stack trace
backtrace full

# Select stack frame
frame 2

# Move up/down stack
up
down

# Show current frame info
info frame
```

### Variable Examination

```bash
# Print variable value
print variable_name
p variable_name

# Print with format
print/x variable_name    # hexadecimal
print/d variable_name    # decimal
print/s variable_name    # string
print/c variable_name    # character

# Print complex expressions
print *pointer_var
print array[index]
print struct_var.field

# Set variable value
set variable_name = value

# Show local variables
info locals

# Show function arguments
info args

# Watch variable for changes
watch variable_name
```

### Memory and Assembly

```bash
# Examine memory
examine 0x401234
x/10x 0x401234          # 10 hex words
x/20i 0x401234          # 20 instructions
x/s 0x401234            # string

# Disassemble
disassemble
disassemble main
disassemble 0x401234

# Show registers
info registers
info all-registers
```

### Source Code

```bash
# List source around current line
list
l

# List specific lines
list 42
list main.goo:42
list function_name

# List range
list 40,50

# Search source
search pattern
reverse-search pattern
```

### Advanced Features

```bash
# Show program info
info program
info files
info functions
info variables

# Thread debugging
info threads
thread 2

# Core dump analysis
info core

# Memory mapping
info proc mappings

# Shared libraries
info shared
```

## Debugging Scenarios

### Basic Program Debugging

```bash
$ goo-debug my_program
(goo-debug) break main
Breakpoint 1 set at 0x401234
(goo-debug) run
Starting program: my_program

Breakpoint 1 hit at 0x401234
=> 15    func main() {
(goo-debug) step
=> 16        x := 42
(goo-debug) print x
x = 42
(goo-debug) continue
Program exited normally
```

### Debugging with Arguments

```bash
$ goo-debug server -- --port 8080 --debug
(goo-debug) break server.goo:initialize
(goo-debug) run
=> 25    func initialize(port int, debug bool) {
(goo-debug) info args
port = 8080
debug = true
```

### Attaching to Running Process

```bash
$ goo-debug -p 1234
Attached to process 1234
Process stopped at 0x7f1234567890
(goo-debug) backtrace
#0  0x7f1234567890 in select()
#1  0x401234 in event_loop() at server.goo:45
#2  0x401156 in main() at server.goo:15
```

### Memory Investigation

```bash
(goo-debug) examine/10x $rsp
0x7fff12345678: 0x401234 0x7fff1234 0x000000 0x401156
0x7fff12345688: 0x000001 0x7fff1234 0x401234 0x000000
0x7fff12345698: 0x7f1234 0x567890

(goo-debug) disassemble $rip,+20
0x401234 <main+4>:   mov    %rdi,-0x8(%rbp)
0x401238 <main+8>:   mov    %rsi,-0x10(%rbp)
0x40123c <main+12>:  callq  0x401156 <initialize>
```

### Variable Inspection

```bash
(goo-debug) info locals
x = 42
y = ?int(nil)
message = "Hello, World!"
numbers = {1, 2, 3, 4, 5}

(goo-debug) print numbers[2]
numbers[2] = 3

(goo-debug) print/x &message
&message = 0x7fff12345678

(goo-debug) set x = 100
(goo-debug) print x
x = 100
```

## Goo Language Features

### Error Union Debugging

```goo
func risky_operation() -> !Result {
    return ErrorNotFound
}
```

```bash
(goo-debug) break risky_operation
(goo-debug) continue
=> 42    return ErrorNotFound
(goo-debug) print $return_value
$return_value = !Result{error: ErrorNotFound}
(goo-debug) info error_state
Current error: ErrorNotFound
```

### Nullable Type Debugging

```goo
var maybe_value: ?int = nil
if maybe_value != nil {
    fmt.Println(*maybe_value)
}
```

```bash
(goo-debug) break nullable_check
(goo-debug) print maybe_value
maybe_value = ?int(nil)
(goo-debug) set maybe_value = 42
(goo-debug) print maybe_value
maybe_value = ?int(42)
```

### Ownership Debugging

```goo
func transfer_ownership(data: Vec[int]) {
    // data is moved here
}
```

```bash
(goo-debug) break transfer_ownership
(goo-debug) info ownership
data: owned by current function
(goo-debug) step
(goo-debug) info ownership
data: moved (no longer accessible)
```

### Channel Debugging

```goo
ch := make(chan int, 10)
go sender(ch)
value := <-ch
```

```bash
(goo-debug) info channels
ch: buffered channel, capacity=10, length=3
(goo-debug) info goroutines
Goroutine 1: main.main() at main.goo:15
Goroutine 2: main.sender() at main.goo:25 (blocked on channel send)
```

## Configuration

### Startup Configuration

Create `~/.goo-debug` configuration file:

```bash
# Automatically list source on stop
set auto-list-source on

# Show registers on stop
set print-registers off

# History size
set history-size 1000

# Breakpoint auto-enable
set breakpoint-auto-enable on
```

### Environment Variables

- `GOO_DEBUG_PATH` - Additional search paths for source files
- `GOO_DEBUG_SYMBOLS` - Additional symbol directories
- `GOO_DEBUG_TRACE` - Enable tracing output

## Integration

### IDE Integration

#### VS Code

Install the Goo debugging extension or configure as external debugger:

```json
{
    "type": "goo",
    "request": "launch",
    "name": "Debug Goo Program",
    "program": "${workspaceFolder}/main.goo",
    "args": [],
    "console": "integratedTerminal"
}
```

#### Vim/Neovim

Use with debugging plugins like `vimspector` or `nvim-dap`:

```lua
require('dap').adapters.goo = {
    type = 'executable',
    command = 'goo-debug',
    args = {'--dap-mode'}
}
```

### Build System Integration

Generate debug information during compilation:

```bash
# Enable debug symbols
goo build -g main.goo

# Disable optimizations for better debugging
goo build -O0 -g main.goo

# Generate DWARF-4 format
goo build -gdwarf-4 main.goo
```

## Advanced Features

### Core Dump Analysis

```bash
# Generate core dump
ulimit -c unlimited
goo-debug -c core.dump my_program

# Analyze crash
(goo-debug) backtrace
(goo-debug) info registers
(goo-debug) examine crash_address
```

### Remote Debugging

```bash
# Start debug server
goo-debug --server --port 2345 my_program

# Connect from remote debugger
goo-debug --remote host:2345
```

### Scripted Debugging

Create debug scripts for automated testing:

```bash
# debug_script.gdb
break main
run
print variable_name
continue
quit
```

```bash
goo-debug --batch --command=debug_script.gdb my_program
```

## Performance

### Optimization for Debugging

- **Symbol Loading** - Lazy loading of DWARF symbols
- **Breakpoint Management** - Efficient breakpoint insertion/removal
- **Memory Caching** - Cache frequently accessed memory regions
- **Step Optimization** - Smart stepping over library code

### Benchmarks

Typical performance characteristics:

- **Startup Time** - < 100ms for programs with debug info
- **Breakpoint Hit** - < 10ms overhead
- **Variable Inspection** - < 50ms for complex structures
- **Memory Usage** - ~5MB base + symbol table size

## Troubleshooting

### Common Issues

1. **No Debug Information**
   ```bash
   # Solution: Compile with debug flags
   goo build -g program.goo
   ```

2. **Source Not Found**
   ```bash
   # Solution: Set source search paths
   set substitute-path /old/path /new/path
   ```

3. **Optimized Code**
   ```bash
   # Solution: Disable optimizations
   goo build -O0 program.goo
   ```

4. **Permission Denied**
   ```bash
   # Solution: Run with appropriate privileges
   sudo goo-debug program
   ```

### Debug Logging

Enable detailed logging:

```bash
export GOO_DEBUG_TRACE=1
goo-debug --verbose program 2>debug.log
```

## Testing

### Run Test Suite

```bash
make test
```

### Manual Testing

```bash
# Build test program
make test-program

# Run debugger tests
./test_debugger.sh

# Interactive testing
goo-debug test_program
```

## Contributing

### Development Setup

```bash
git clone <repository>
cd tools/debug
make debug  # Build with debug symbols
```

### Adding Features

1. **DWARF Support** - Enhance DWARF information parsing
2. **Language Features** - Add support for new Goo constructs
3. **Performance** - Optimize debugging operations
4. **Platform Support** - Add support for additional architectures

### Testing

- Create comprehensive test cases
- Test with various program types
- Verify DWARF compatibility
- Performance regression testing

## License

Part of the Goo programming language toolchain.
Copyright (c) 2024 Goo Language Project.