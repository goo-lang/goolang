# 🛠️ Goo CLI Tool Template

A robust command-line application template showcasing Goo's error handling and safety features.

## ✨ Features

- **Safe Argument Parsing**: Error unions for command-line validation
- **Nullable Options**: Optional output files with safe access
- **Batch Processing**: Process multiple files with error collection
- **Comprehensive Error Handling**: Never crash, always provide feedback
- **Verbose Mode**: Optional detailed output

## 🚀 Quick Start

```bash
# Create new CLI tool from template
goo new cli-tool my-tool

# Build and test
cd my-tool
goo build
./my-tool --help
```

## 💡 Usage Examples

### Basic Usage
```bash
# Process single file
./my-tool document.txt

# Process multiple files
./my-tool file1.txt file2.txt file3.txt
```

### Advanced Usage
```bash
# Verbose mode with output file
./my-tool -verbose -output results.txt *.txt

# Process all markdown files
./my-tool -verbose *.md
```

## 🔧 Template Features

### Error Union Pattern
```goo
func processFile(filepath string) (!string, !Error) {
    if !path.Exists(filepath) {
        return nil, Error("File not found")
    }
    // ... processing logic
    return result, nil
}
```

### Nullable Types for Optional Values
```goo
type Config struct {
    OutputFile ?string  // Optional output file
    Verbose    bool     // Required flag
}

if config.OutputFile? {
    writeToFile(config.OutputFile!)
}
```

### Safe Error Collection
```goo
func processFiles(files []string) ProcessResult {
    var errors []string
    
    for _, file := range files {
        result, err := processFile(file)
        if err! {
            errors = append(errors, err!.String())
            continue
        }
        // Handle success...
    }
    
    return ProcessResult{Errors: errors}
}
```

## 🎯 Customization Guide

### 1. Add New Command Line Options
```goo
flag.StringVar(&config.Format, "format", "json", "Output format")
flag.IntVar(&config.MaxFiles, "max", 100, "Maximum files to process")
```

### 2. Implement Custom Processing Logic
```goo
func processFile(filepath string, config Config) (!ProcessedFile, !Error) {
    // Your custom processing logic here
    // Return processed result or error
}
```

### 3. Add Progress Reporting
```goo
func processFiles(config Config) ProcessResult {
    total := len(config.InputFiles)
    
    for i, filepath := range config.InputFiles {
        fmt.Printf("Progress: %d/%d (%s)\n", i+1, total, filepath)
        // ... processing
    }
}
```

### 4. Add Configuration File Support
```goo
type Config struct {
    // ... existing fields
    ConfigFile ?string
}

func loadConfig(path string) (!Config, !Error) {
    // Load from TOML/JSON file
}
```

## 📚 Key Concepts Demonstrated

1. **Error Unions**: Functions return either success or error, never both
2. **Nullable Types**: Optional values with compile-time null safety
3. **Pattern Matching**: Safe unwrapping with `if value!` syntax
4. **Memory Safety**: No segfaults or null pointer dereferences
5. **Explicit Error Handling**: All errors must be handled explicitly

## 🧪 Testing

```bash
# Test with various inputs
./my-tool nonexistent.txt        # Should handle missing file gracefully
./my-tool -output /dev/null *.go # Should process all .go files
./my-tool -verbose large-file.txt # Should show processing details
```

## 🔄 Next Steps

1. Add configuration file support
2. Implement progress bars for long operations
3. Add colored output for better UX
4. Create man page documentation
5. Add shell completion scripts
6. Package for distribution (homebrew, apt, etc.)