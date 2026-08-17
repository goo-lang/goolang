# Time-Travel Debugging System

The Goo compiler includes a comprehensive time-travel debugging system that allows developers to step backward and forward through execution history, inspect program state at different points in time, and understand how their code behaves during execution.

## Overview

Time-travel debugging captures execution history in snapshots, allowing you to:

- **Step backward** through execution to see how variables changed
- **Navigate to specific points** in execution history
- **Inspect program state** at any captured moment
- **Search for specific events** like function calls or errors
- **Visualize execution timeline** with detailed information

## Key Features

### 1. **Execution History Tracking**
- Automatic snapshot creation at key execution points
- Manual snapshot creation for debugging milestones
- Configurable snapshot frequency and filtering
- Memory-efficient snapshot storage with automatic cleanup

### 2. **Time-Travel Navigation**
- Step backward/forward through execution history
- Jump to specific execution steps or timestamps
- Continue to beginning/end of execution
- Search-based navigation to find specific events

### 3. **State Inspection**
- Variable values at any point in execution
- Call stack information with function context
- Memory usage tracking (heap and stack)
- Performance metrics per execution step

### 4. **REPL Integration**
- Interactive debugging commands in the REPL
- Real-time timeline visualization
- Seamless integration with error reporting
- Easy navigation between execution states

## REPL Commands

### Enable/Disable Debugging

```goo
goo> :debug enable
Time-travel debugging enabled
Debug session started

goo> :debug disable
Time-travel debugging disabled
Debug session ended
```

### Navigation Commands

#### Step Through Execution
```goo
goo> :debug step back
Step backward to step 42

goo> :debug step forward  
Step forward to step 43
```

#### Jump to Specific Points
```goo
goo> :debug goto 25
Jumped to step 25

goo> :debug continue back
Continued to beginning (step 0)

goo> :debug continue forward
Continued to end (step 100)
```

### State Inspection

#### View Execution Timeline
```goo
goo> :debug timeline
=== Debug Timeline ===
Total snapshots: 15
Total steps: 100

Step   | Event Type          | Description
-------|---------------------|----------------------------------
      0 | Expression Start    | Starting expression evaluation
      5 | Function Call       | Calling function: calculate
     12 | Variable Assignment | Variable x assigned value 42
     18 | Error               | Type mismatch in assignment
→    25 | Manual Snapshot     | Checkpoint before complex calculation
     30 | Expression End      | Finished expression evaluation
```

#### Current Debug State
```goo
goo> :debug state
=== Current Debug State ===
Step: 25
Event: Manual Snapshot
Description: Checkpoint before complex calculation
Expression: let result = calculate(x, y)
Location: main.goo:15:8
Memory usage: Heap=1024, Stack=512
Performance: CPU=2.35ms, Instructions=1543
```

#### Variable Inspection
```goo
goo> :debug variables
=== Variables ===
Global Variables:
  config: Config{debug: true, version: "1.0"}
  
Local Variables:
  x: 42
  y: 3.14
  result: <undefined>
```

#### Call Stack
```goo
goo> :debug stack
=== Call Stack ===
#0 calculate at math.goo:25:4
#1 main at main.goo:15:8
```

### Search and Discovery

#### Find Specific Events
```goo
goo> :debug find error
Found error at step 18

goo> :debug find function calculate
Found function 'calculate' at step 5

goo> :debug find variable result
Found variable 'result' change at step 30
```

#### Create Manual Snapshots
```goo
goo> :debug snapshot "Before optimization"
Created manual snapshot: Before optimization

goo> :debug snapshot
Created manual snapshot: Manual REPL snapshot
```

## Configuration Options

### Snapshot Behavior
```goo
goo> :debug enable
goo> :set debug.auto_snapshot true
goo> :set debug.snapshot_frequency 10  // Every 10 steps
goo> :set debug.max_snapshots 1000     // Keep last 1000 snapshots
```

### Event Filtering
```goo
goo> :set debug.snapshot_on_errors true
goo> :set debug.snapshot_on_function_calls true
goo> :set debug.snapshot_on_assignments false
goo> :set debug.snapshot_on_control_flow false
```

## Usage Examples

### Basic Debugging Workflow

```goo
goo> :debug enable
Time-travel debugging enabled

goo> let x = 10
x: int = 10

goo> let y = x * 2 + 5
y: int = 25

goo> :debug timeline
=== Debug Timeline ===
Total snapshots: 4
Total steps: 2

Step   | Event Type          | Description
-------|---------------------|----------------------------------
      0 | Expression Start    | Starting expression evaluation
      1 | Expression End      | Finished expression evaluation
      1 | Expression Start    | Starting expression evaluation
→     2 | Expression End      | Finished expression evaluation

goo> :debug step back
Step backward to step 1

goo> :debug variables
=== Variables ===
Local Variables:
  x: 10
  y: <undefined>

goo> :debug step forward
Step forward to step 2

goo> :debug variables
=== Variables ===
Local Variables:
  x: 10
  y: 25
```

### Debugging Function Calls

```goo
goo> fn factorial(n: int) -> int {
...>   if n <= 1 { return 1; }
...>   return n * factorial(n - 1);
...> }

goo> :debug enable
goo> let result = factorial(5)

goo> :debug find function factorial
Found function 'factorial' at step 15

goo> :debug state
=== Current Debug State ===
Step: 15
Event: Function Call
Description: Calling function: factorial
Expression: factorial(5)
Location: <repl>:1:14
```

### Error Investigation

```goo
goo> let x: int = "hello"  // Type error
error: Type mismatch in assignment

goo> :debug timeline
=== Debug Timeline ===
Step   | Event Type          | Description
-------|---------------------|----------------------------------
→    10 | Error               | Error occurred: Type mismatch in assignment

goo> :debug state
=== Current Debug State ===
Step: 10
Event: Error
Description: Error occurred: Type mismatch in assignment
Expression: let x: int = "hello"
Location: <repl>:1:14

goo> :debug find error
Found error at step 10
```

## Advanced Features

### Timeline Visualization

The timeline shows execution flow with different event types:
- **Expression Start/End**: Code evaluation boundaries
- **Function Call/Return**: Function invocation tracking
- **Variable Assignment**: Variable state changes
- **Control Flow**: Branches, loops, and jumps
- **Error**: Runtime and compile-time errors
- **Manual Snapshot**: User-created checkpoints

### Memory Tracking

Each snapshot includes memory usage information:
- **Heap Usage**: Dynamic memory allocation
- **Stack Usage**: Call stack and local variables
- **Memory Layout**: Serialized memory state (future feature)

### Performance Metrics

Snapshots capture performance data:
- **CPU Time**: Execution time for each step
- **Instruction Count**: Low-level execution metrics
- **Timeline Statistics**: Overall execution analysis

### Search Capabilities

Find specific events in execution history:
- **Event Type Search**: Find errors, function calls, etc.
- **Variable Search**: Track variable changes over time
- **Function Search**: Locate specific function invocations
- **Location Search**: Find execution at specific source locations

## Integration with Other Systems

### Error Reporting
Time-travel debugging integrates with the enhanced error reporting system:
- Automatic snapshots on errors
- Error context preservation
- Timeline-based error analysis

### Performance Monitoring
Integration with real-time performance monitoring:
- Performance metrics in snapshots
- Execution time tracking
- Resource usage analysis

### Hot Reload
Seamless integration with hot reload:
- Snapshot preservation across reloads
- State restoration after code changes
- Development workflow continuity

## Best Practices

### For Developers

1. **Enable Early**: Start debugging sessions at the beginning of development
2. **Use Manual Snapshots**: Create checkpoints before complex operations
3. **Search Efficiently**: Use find commands to locate specific events quickly
4. **Check State Regularly**: Use `:debug state` to understand current context
5. **Monitor Performance**: Watch execution time and memory usage

### For Debugging Sessions

1. **Start with Timeline**: Get overview with `:debug timeline`
2. **Navigate Systematically**: Use step commands to understand flow
3. **Inspect Variables**: Check variable state at key points
4. **Search for Patterns**: Use find commands to locate recurring issues
5. **Create Snapshots**: Mark important states for later reference

### Performance Considerations

1. **Configure Frequency**: Adjust snapshot frequency for performance
2. **Filter Events**: Enable only necessary event types
3. **Limit History**: Set appropriate maximum snapshot counts
4. **Clean Up**: Regular session restarts for long debugging sessions

## Implementation Details

### Snapshot Structure
Each snapshot contains:
- Execution metadata (step, timestamp, event type)
- Program state (variables, call stack, location)
- Performance metrics (CPU time, memory usage)
- Navigation links (previous/next snapshots)

### Memory Management
- Automatic cleanup of old snapshots
- Configurable memory limits
- Efficient storage of program state
- Copy-on-write for unchanged data

### Timeline Navigation
- Bidirectional linked list for efficient navigation
- O(1) step forward/backward operations
- O(n) search operations with early termination
- Timeline compression for long-running sessions

The time-travel debugging system transforms debugging from a frustrating trial-and-error process into a systematic exploration of program execution, helping developers understand exactly how their code behaves and identify issues more effectively.