# Hot Reload System

The Goo Hot Reload System enables live code updates without restarting your application, dramatically improving development productivity.

## Overview

Hot reload allows you to modify functions, types, and modules while your program is running. Changes are automatically detected, compiled, and applied with state preservation.

## Features

### 1. Function Hot Reload
Mark functions with `@hot_reload` to enable live updates:

```goo
@hot_reload
func renderUI(data: AppState) {
    // Modify this function while running!
    fmt.Printf("Counter: %d\n", data.counter)
}
```

### 2. Type Migration
When types change, provide migration logic:

```goo
@migrate(UserData)
func migrateUserData(old: UserData, new: *UserData) bool {
    new.id = old.id
    new.name = old.name
    // Initialize new fields with defaults
    new.createdAt = time.Now()
    return true
}
```

### 3. State Preservation
Application state is automatically preserved across reloads:

- Primitive types are copied directly
- Complex types use serialization
- Custom migration for incompatible changes

### 4. File Watching
Automatic detection of source file changes:

```goo
hotReload.watchFiles("src/*.goo")
hotReload.watchDirectory("src/components")
```

### 5. Safe Points
Ensure reloads happen at safe execution points:

```goo
hotReload.enterSafePoint()
defer hotReload.exitSafePoint()
// Critical section - no reloads here
```

## Usage

### Basic Setup

```goo
import "runtime/hotreload"

func main() {
    // Enable hot reload
    hotReload.enable()
    
    // Watch source files
    hotReload.watchFiles("src/*.goo")
    
    // Your application code
    runApp()
}
```

### Marking Hot-Reloadable Code

```goo
// Functions
@hot_reload
func processData(input: []byte) Result {
    // Function implementation
}

// Types (with migration)
@hot_reload
type Config struct {
    version int
    settings map[string]string
}

@migrate(Config)
func migrateConfig(old: Config, new: *Config) bool {
    // Migration logic
    return true
}
```

### Handling Reload Events

```goo
// Check for changes
if hotReload.hasChanges() {
    // Optional: Show reload UI
    showReloadingIndicator()
    
    // Perform reload
    if err := hotReload.reload(); err != nil {
        log.Error("Reload failed:", err)
    } else {
        log.Info("Code reloaded successfully")
    }
}
```

## Configuration

### Compiler Options

```goo
hotReload.configure({
    // Maximum preserved state versions
    maxStateVersions: 10,
    
    // Reload timeout
    reloadTimeout: 5 * time.Second,
    
    // Safety level
    safetyLevel: hotReload.SafetyBalanced,
    
    // Enable specific capabilities
    capabilities: hotReload.CapFunction | hotReload.CapType,
})
```

### Performance Tuning

```goo
// Batch reloads for better performance
hotReload.batchMode(true)
defer hotReload.batchMode(false)

// Multiple file changes will reload together
modifyMultipleFiles()
```

## Best Practices

### 1. Design for Reloadability

- Keep functions focused and independent
- Minimize global state
- Use dependency injection
- Implement clear interfaces

### 2. State Management

```goo
// Good: Explicit state container
type AppState struct {
    mu       sync.Mutex
    data     map[string]interface{}
    version  int
}

// Bad: Scattered global variables
var (
    counter int
    users   []User
    config  Config
)
```

### 3. Migration Strategies

```goo
// Version your types for easier migration
type UserV2 struct {
    version int // Always include version
    id      string
    name    string
    email   string // New field in V2
}

@migrate(UserV2)
func migrateUser(old: interface{}, new: *UserV2) bool {
    switch v := old.(type) {
    case UserV1:
        new.id = v.id
        new.name = v.name
        new.email = "" // Default for new field
        return true
    default:
        return false
    }
}
```

### 4. Error Handling

```goo
// Set up reload error handler
hotReload.onError(func(err error) {
    log.Error("Hot reload error:", err)
    
    // Optionally fall back to restart
    if err.IsCritical() {
        gracefulRestart()
    }
})
```

## Limitations

1. **Not Suitable For:**
   - System-level code
   - Security-critical paths
   - Real-time systems with strict timing

2. **Performance Impact:**
   - Small overhead for change detection
   - Compilation time for large modules
   - Memory usage for state preservation

3. **Compatibility:**
   - Some language features may not be reloadable
   - External C libraries cannot be reloaded
   - Interface changes require careful migration

## Troubleshooting

### Common Issues

1. **Reload Fails**
   ```goo
   // Check compilation errors
   if err := hotReload.getLastError(); err != nil {
       fmt.Printf("Compilation error: %v\n", err)
   }
   ```

2. **State Loss**
   ```goo
   // Ensure proper migration
   hotReload.setMigrationVerbose(true)
   ```

3. **Performance Degradation**
   ```goo
   // Monitor reload statistics
   stats := hotReload.getStatistics()
   fmt.Printf("Avg reload time: %.2fms\n", stats.avgReloadTime)
   ```

## Advanced Features

### Custom Reload Handlers

```goo
// Register pre-reload hook
hotReload.beforeReload(func() {
    saveApplicationState()
    pauseBackgroundTasks()
})

// Register post-reload hook
hotReload.afterReload(func() {
    restoreApplicationState()
    resumeBackgroundTasks()
})
```

### Conditional Reloading

```goo
// Only reload specific modules
hotReload.setFilter(func(path string) bool {
    return strings.HasPrefix(path, "src/ui/")
})
```

### Integration with IDE

The hot reload system integrates with IDEs through the Language Server Protocol (LSP), providing:

- Live error highlighting
- Reload status indicators
- One-click reload actions
- Automatic reload on save

## Examples

See the `examples/hot_reload_demo.goo` file for a complete working example of hot reload in action.