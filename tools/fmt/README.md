# Goo Code Formatter (goo-fmt)

A configurable code formatter for the Goo programming language that enforces consistent style and formatting across codebases.

## Features

- **Configurable formatting rules** - Customize indentation, spacing, and layout
- **Multiple output modes** - Format to stdout, in-place, or to specific files
- **Recursive directory processing** - Format entire projects at once
- **Check mode** - Verify code formatting without making changes
- **Diff mode** - Show formatting changes before applying
- **Configuration file support** - Project-specific formatting rules
- **Fast and reliable** - Handles large codebases efficiently

## Installation

Build the formatter from source:

```bash
cd tools/fmt
make
```

Install system-wide:

```bash
make install PREFIX=/usr/local
```

## Usage

### Basic Usage

```bash
# Format file and print to stdout
goo-fmt main.goo

# Format file in place
goo-fmt -w main.goo

# Format multiple files
goo-fmt -w *.goo

# Format directory recursively
goo-fmt -r -w src/
```

### Check Mode

```bash
# Check if files need formatting (exits 1 if changes needed)
goo-fmt -c main.goo

# Check entire project
goo-fmt -c -r .
```

### Configuration

```bash
# Use custom configuration file
goo-fmt --config my-style.toml main.goo

# Override specific settings
goo-fmt --indent 2 --tabs main.goo
goo-fmt --max-line 120 main.goo
```

### Output Options

```bash
# Write to specific file
goo-fmt -o formatted.goo main.goo

# Show diff of changes
goo-fmt -d main.goo
```

## Command Line Options

### General Options
- `-h, --help` - Show help message
- `-w, --write` - Write result to source file instead of stdout
- `-r, --recursive` - Process directories recursively
- `-c, --check` - Check if files are formatted (exit 1 if not)
- `-d, --diff` - Show diff of formatting changes
- `-o, --output <file>` - Write output to specified file

### Configuration Options
- `--config <file>` - Use specified configuration file
- `--indent <size>` - Set indentation size (default: 4)
- `--tabs` - Use tabs for indentation instead of spaces
- `--max-line <length>` - Set maximum line length (default: 100)

## Configuration Files

The formatter looks for configuration in the following order:

1. File specified with `--config`
2. `.goo-fmt.toml` in current directory
3. `~/.config/goo/fmt.toml`

### Configuration Format

```toml
# Indentation settings
indent_size = 4
tab_width = 4
use_tabs = false

# Line length
max_line_length = 100

# Spacing rules
space_after_comma = true
space_around_operators = true
space_before_paren = false
space_inside_paren = false
space_before_brace = true
space_inside_brace = false

# Brace placement
newline_before_brace = false

# Alignment
align_struct_fields = true
align_function_params = true

# Import management
sort_imports = true

# Whitespace cleanup
remove_trailing_whitespace = true
ensure_newline_at_eof = true

# Blank lines
blank_lines_around_functions = true
blank_lines_around_types = true

# Compactness
compact_short_blocks = true
```

## Formatting Rules

### Indentation
- Configurable indent size (default: 4 spaces)
- Option to use tabs instead of spaces
- Consistent indentation for nested blocks

### Spacing
- Space after commas: `func(a, b, c)`
- Space around operators: `x + y * z`
- Configurable spacing around parentheses and braces
- No trailing whitespace

### Brace Style
- Configurable brace placement (same line or new line)
- Consistent spacing before opening braces
- Proper indentation for block contents

### Function Formatting
```goo
// Before
func add(a:int,b:int)->int{return a+b}

// After
func add(a: int, b: int) -> int {
    return a + b
}
```

### Struct Formatting
```goo
// Before
type Person struct{name:string;age:int}

// After
type Person struct {
    name: string
    age:  int
}
```

### Type Annotations
```goo
// Error unions and nullable types
func riskyOperation() -> !Result {
    value := ?int(42)
    if value != nil {
        return Ok(*value)
    }
    return Err("not found")
}
```

### Import Sorting
```goo
// Before
import "strings"
import "fmt"
import "os"

// After (sorted)
import "fmt"
import "os" 
import "strings"
```

## Integration

### Editor Integration

The formatter can be integrated with various editors:

**VS Code**: Use the `--check` mode in save hooks
**Vim**: Set up autocommands to run formatter on save
**Emacs**: Configure format-on-save with the formatter

### CI/CD Integration

```bash
# Check formatting in CI
goo-fmt -c -r .
if [ $? -ne 0 ]; then
    echo "Code is not properly formatted"
    echo "Run: goo-fmt -w -r ."
    exit 1
fi
```

### Pre-commit Hooks

```bash
#!/bin/sh
# Pre-commit hook to format Goo code
goo-fmt -w $(git diff --cached --name-only --diff-filter=ACM | grep '\.goo$')
```

## Examples

### Example Input
```goo
package main
import "fmt"
func main(){x:=10;y:=20;fmt.Printf("Sum: %d\n",x+y)}
type Person struct{name:string;age:int}
```

### Example Output
```goo
package main

import "fmt"

func main() {
    x := 10
    y := 20
    fmt.Printf("Sum: %d\n", x + y)
}

type Person struct {
    name: string
    age:  int
}
```

## Testing

Run the test suite:

```bash
make test
```

Test specific functionality:

```bash
# Test basic formatting
echo 'func main(){fmt.Println("Hello")}' | goo-fmt

# Test configuration
goo-fmt --indent 2 --tabs test.goo

# Test check mode
goo-fmt -c unformatted.goo
```

## Performance

The formatter is designed for speed and handles large codebases efficiently:

- **Fast tokenization** - Efficient lexical analysis
- **Single-pass formatting** - Minimal memory usage
- **Parallel processing** - Can format multiple files concurrently
- **Incremental updates** - Only formats changed files in CI

Typical performance on modern hardware:
- Small files (< 1KB): < 1ms
- Medium files (1-100KB): 1-10ms  
- Large files (> 100KB): 10-100ms

## Compatibility

The formatter handles all Goo language constructs:

- **Standard Go syntax** - Full compatibility with Go code
- **Goo extensions** - Error unions, nullable types, ownership
- **Channel patterns** - pub/sub, req/rep patterns
- **Unsafe blocks** - Hardware control primitives
- **Comments** - Preserves all comment styles

## Contributing

To contribute to the formatter:

1. Add test cases for new formatting rules
2. Update configuration options as needed
3. Ensure backward compatibility
4. Test with real-world codebases

## License

Part of the Goo programming language toolchain.
Copyright (c) 2024 Goo Language Project.