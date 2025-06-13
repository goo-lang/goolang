# Goo Testing Strategy

## Overview

The Goo testing strategy ensures language correctness, safety guarantees, and performance targets through comprehensive automated testing at every level. We adopt a "test everything" philosophy with zero tolerance for regressions.

## Testing Levels

### 1. Unit Testing

#### Compiler Components

```rust
// lexer_test.rs
#[test]
fn test_tokenize_integers() {
    let input = "123 0xFF 0b1010 0o777";
    let tokens = tokenize(input).unwrap();
    
    assert_eq!(tokens[0], Token::IntLit(123, Base::Decimal));
    assert_eq!(tokens[1], Token::IntLit(255, Base::Hex));
    assert_eq!(tokens[2], Token::IntLit(10, Base::Binary));
    assert_eq!(tokens[3], Token::IntLit(511, Base::Octal));
}

#[test]
fn test_unicode_identifiers() {
    let input = "let 名前 = \"太郎\"";
    let tokens = tokenize(input).unwrap();
    assert_eq!(tokens[1], Token::Ident("名前".to_string()));
}

#[test]
fn test_error_recovery() {
    let input = "let x = @#$";
    let result = tokenize(input);
    assert!(result.is_err());
    assert!(result.unwrap_err().contains("Invalid character"));
}
```

#### Type System

```rust
// type_checker_test.rs
#[test]
fn test_type_inference() {
    let ast = parse("let x = 42").unwrap();
    let typed = type_check(ast).unwrap();
    
    assert_eq!(
        typed.get_type("x"),
        Type::Primitive(PrimitiveType::I32)
    );
}

#[test]
fn test_generic_function_instantiation() {
    let code = r#"
        func identity<T>(x: T) T { x }
        let a = identity(42)
        let b = identity("hello")
    "#;
    
    let typed = type_check(parse(code).unwrap()).unwrap();
    assert_eq!(typed.get_type("a"), Type::Primitive(PrimitiveType::I32));
    assert_eq!(typed.get_type("b"), Type::Primitive(PrimitiveType::String));
}
```

### 2. Integration Testing

#### End-to-End Compilation

```goo
// test/integration/compilation_test.goo
#[test]
func test_compile_hello_world() {
    let code = `
        func main() {
            print("Hello, World!")
        }
    `
    
    let binary = compile_to_binary(code)
    let output = run_binary(binary)
    
    assert_eq!(output.stdout, "Hello, World!\n")
    assert_eq!(output.exit_code, 0)
}

#[test]
func test_compile_with_optimization() {
    let code = load_file("benchmarks/matrix_multiply.goo")
    
    let debug_binary = compile(code, OptLevel::Debug)
    let release_binary = compile(code, OptLevel::Release)
    
    // Release binary should be smaller
    assert!(size_of(release_binary) < size_of(debug_binary))
    
    // But produce same output
    assert_eq!(
        run_binary(debug_binary).stdout,
        run_binary(release_binary).stdout
    )
}
```

#### Runtime Behavior

```goo
// test/integration/concurrency_test.goo
#[test(timeout = 5s)]
func test_channel_communication() {
    let ch = channel<int>()
    
    go func() {
        ch <- 42
    }()
    
    let value = <-ch
    assert_eq!(value, 42)
}

#[test]
func test_actor_supervision() {
    let supervisor = TestSupervisor.spawn()
    let worker = supervisor.spawn_child(FailingWorker)
    
    // Worker should crash
    worker ! CrashNow()
    sleep(100ms)
    
    // But be restarted by supervisor
    let response = worker ? GetStatus()
    assert_eq!(response, Status::Running)
    assert_eq!(supervisor.get_restart_count(worker), 1)
}
```

### 3. Property-Based Testing

#### Compiler Properties

```goo
#[property_test(cases = 10000)]
func prop_parse_print_roundtrip(program: ValidProgram) {
    let ast = parse(program.to_string())
    let printed = ast.to_string()
    let reparsed = parse(printed)
    
    assert_eq!(ast, reparsed)
}

#[property_test]
func prop_type_safety(program: TypedProgram) {
    // If type checks pass, program cannot crash with type error
    if let Ok(typed) = type_check(program) {
        let result = execute(typed)
        assert!(!result.is_type_error())
    }
}

#[property_test]
func prop_optimization_preserves_semantics(program: ValidProgram) {
    let unoptimized = compile(program, OptLevel::None)
    let optimized = compile(program, OptLevel::Max)
    
    assert_eq!(
        execute(unoptimized),
        execute(optimized)
    )
}
```

#### Runtime Properties

```goo
#[property_test]
func prop_channel_no_data_loss(
    messages: Vec<int>,
    buffer_size: 0..100
) {
    let ch = channel<int>(buffer_size)
    let received = Arc<Mutex<Vec<int>>>.new(vec[])
    
    // Send all messages
    go func() {
        for msg in messages {
            ch <- msg
        }
        close(ch)
    }()
    
    // Receive all messages
    go func() {
        for msg in ch {
            received.lock().push(msg)
        }
    }()
    
    wait_for_completion()
    assert_eq!(messages.sort(), received.lock().sort())
}
```

### 4. Fuzzing

#### Parser Fuzzing

```rust
// fuzz/fuzz_targets/parser.rs
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if let Ok(s) = std::str::from_utf8(data) {
        // Parser should never panic
        let _ = goo::parse(s);
    }
});
```

#### Type System Fuzzing

```rust
fuzz_target!(|program: ArbitraryProgram| {
    // Type checker should never panic
    if let Ok(ast) = parse(&program.to_string()) {
        let _ = type_check(ast);
    }
});
```

#### Runtime Fuzzing

```goo
#[fuzz]
func fuzz_allocator(operations: []AllocOp) {
    let allocator = Arena.new(1MB)
    
    for op in operations {
        match op {
            Alloc(size) => {
                let _ = allocator.alloc(size)
            },
            Free(ptr) => {
                allocator.free(ptr)
            },
            Realloc(ptr, size) => {
                let _ = allocator.realloc(ptr, size)
            }
        }
    }
    
    // Should never corrupt memory
    allocator.verify_integrity()
}
```

### 5. Performance Testing

#### Micro-benchmarks

```goo
#[bench]
func bench_channel_send_recv(b: Bencher) {
    let ch = channel<int>()
    
    b.iter(|| {
        go func() { ch <- 42 }()
        let _ = <-ch
    })
}

#[bench]
func bench_hashmap_insert(b: Bencher) {
    let map = HashMap<int, int>.new()
    let mut i = 0
    
    b.iter(|| {
        map.insert(i, i)
        i += 1
    })
}

#[bench_group(compare_with = ["rust", "go", "cpp"])]
func bench_matrix_multiply(b: Bencher) {
    let a = Matrix.random(100, 100)
    let b = Matrix.random(100, 100)
    
    b.iter(|| {
        let _ = a * b
    })
}
```

#### Macro-benchmarks

```goo
#[bench(profile = true)]
func bench_web_server_throughput(b: Bencher) {
    let server = TestServer.start()
    let client = TestClient.new()
    
    b.throughput(|| {
        client.get("/api/endpoint")
    })
    
    b.report_metric("requests_per_sec", b.iterations / b.elapsed)
    b.report_metric("p99_latency_ms", b.percentile(99))
}

#[bench]
func bench_compile_time(b: Bencher) {
    let small = load_project("small_project/")  // 1K LOC
    let medium = load_project("medium_project/") // 10K LOC
    let large = load_project("large_project/")   // 100K LOC
    
    b.iter_custom(|iters| {
        let mut total = Duration::zero()
        for _ in 0..iters {
            let start = Instant::now()
            compile_project(medium)
            total += start.elapsed()
            clean_cache()
        }
        total
    })
}
```

### 6. Stress Testing

#### Concurrency Stress

```goo
#[test(stress)]
func stress_concurrent_channels() {
    const PRODUCERS = 1000
    const CONSUMERS = 1000
    const MESSAGES = 10000
    
    let ch = channel<int>(100)
    let received = ConcurrentHashSet<int>.new()
    
    // Spawn producers
    for p in 0..PRODUCERS {
        go func(id: int) {
            for i in 0..MESSAGES {
                ch <- id * MESSAGES + i
            }
        }(p)
    }
    
    // Spawn consumers  
    for _ in 0..CONSUMERS {
        go func() {
            loop {
                select {
                    case msg = <-ch:
                        received.insert(msg)
                    case <-timeout(1s):
                        return
                }
            }
        }()
    }
    
    wait_for_completion()
    assert_eq!(received.len(), PRODUCERS * MESSAGES)
}
```

#### Memory Stress

```goo
#[test(stress, memory_limit = "1GB")]
func stress_memory_allocation() {
    let allocations = vec[]
    
    // Allocate until we hit the limit
    while current_memory() < 900MB {
        allocations.push(vec[0u8; 1MB])
    }
    
    // Free half
    for i in 0..allocations.len()/2 {
        allocations[i] = vec[]
    }
    
    // Allocate again
    while current_memory() < 900MB {
        allocations.push(vec[0u8; 1MB])
    }
    
    // Should not OOM or corrupt memory
}
```

### 7. Correctness Testing

#### Formal Verification

```goo
#[verify]
func verified_sort<T: Ord>(mut arr: []T) {
    requires: true
    ensures: is_sorted(arr) && same_elements(old(arr), arr)
    
    // Quicksort implementation
    if arr.len() <= 1 { return }
    
    let pivot = partition(arr)
    verified_sort(arr[..pivot])
    verified_sort(arr[pivot+1..])
}

#[verify(smt_solver = "z3")]
func verified_binary_search(arr: []int, target: int) ?usize {
    requires: is_sorted(arr)
    ensures: match result {
        Some(i) => arr[i] == target,
        None => for all i in 0..arr.len(): arr[i] != target
    }
    
    // Binary search implementation
}
```

#### Model Checking

```tla
// specs/channel_model.tla
EXTENDS Integers, Sequences

CONSTANTS NumSenders, NumReceivers, BufferSize

SendMessage(ch, msg) ==
    /\ Len(ch.buffer) < BufferSize
    /\ ch' = [ch EXCEPT !.buffer = Append(@, msg)]

ReceiveMessage(ch) ==
    /\ Len(ch.buffer) > 0  
    /\ ch' = [ch EXCEPT !.buffer = Tail(@)]

SafetyInvariant ==
    /\ Len(channel.buffer) <= BufferSize
    /\ NoDataRace
```

### 8. Regression Testing

#### Regression Suite

```toml
# tests/regressions/issues.toml
[[test]]
issue = 1234
description = "Panic when parsing empty struct"
code = "struct Empty {}"
should_compile = true

[[test]]
issue = 5678
description = "Type inference loop"
code = """
func f<T>(x: T) T { g(x) }
func g<T>(x: T) T { f(x) }
"""
should_error = "Recursive type inference"

[[test]]
issue = 9012
description = "Memory leak in channel"
type = "memory"
code = """
func leak() {
    for i in 0..1000000 {
        let ch = channel<BigStruct>()
        ch <- BigStruct.new()
    }
}
"""
max_memory = "100MB"
```

### 9. Compatibility Testing

#### Version Compatibility

```goo
#[test]
func test_binary_compatibility() {
    // Compile with old version
    let old_binary = compile_with_version("1.0", code)
    
    // Link with new runtime
    let linked = link_with_runtime("1.1", old_binary)
    
    // Should still work
    assert!(execute(linked).is_ok())
}

#[test]
func test_source_compatibility() {
    // Load all examples from v1.0
    let examples = load_examples("v1.0")
    
    for example in examples {
        // Should compile with v1.1
        let result = compile_with_version("1.1", example)
        assert!(result.is_ok(), "Failed: {}", example.name)
    }
}
```

## Testing Infrastructure

### Continuous Integration

```yaml
# .github/workflows/test.yml
name: Test Suite

on: [push, pull_request]

jobs:
  quick-tests:
    runs-on: ubuntu-latest
    timeout: 10m
    steps:
      - name: Unit Tests
        run: cargo test --lib
      
      - name: Integration Tests
        run: goo test tests/integration
      
  full-tests:
    runs-on: [ubuntu-latest, macos-latest, windows-latest]
    timeout: 60m
    steps:
      - name: All Tests
        run: goo test --all
        
      - name: Property Tests
        run: goo test --property --cases 10000
        
      - name: Benchmarks
        run: goo bench --compare-baseline
        
  stress-tests:
    runs-on: stress-runner
    timeout: 6h
    steps:
      - name: Stress Tests
        run: goo test --stress --parallel 32
        
      - name: Memory Tests
        run: goo test --memory-limit 32GB
        
  fuzzing:
    runs-on: fuzzing-cluster
    timeout: 24h
    steps:
      - name: Continuous Fuzzing
        run: |
          cargo fuzz run parser -- -max_total_time=86400
          cargo fuzz run type_checker -- -max_total_time=86400
```

### Test Organization

```
tests/
├── unit/
│   ├── lexer/
│   ├── parser/
│   ├── type_checker/
│   └── codegen/
├── integration/
│   ├── compilation/
│   ├── runtime/
│   └── stdlib/
├── property/
│   ├── compiler/
│   └── runtime/
├── stress/
│   ├── concurrency/
│   ├── memory/
│   └── scale/
├── regression/
│   └── issues/
├── benchmarks/
│   ├── micro/
│   └── macro/
└── fuzzing/
    └── corpus/
```

### Test Execution

```bash
# Run all tests
$ goo test

# Run specific test suites
$ goo test unit
$ goo test integration
$ goo test property --cases 10000

# Run with coverage
$ goo test --coverage
$ goo coverage report

# Run benchmarks
$ goo bench
$ goo bench --compare v1.0.0

# Run stress tests
$ goo test stress --duration 1h

# Run fuzzing
$ goo fuzz parser --time 24h
```

### Test Reporting

```
Test Results:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Unit Tests:        1,234 passed, 0 failed
Integration:         567 passed, 0 failed  
Property:         10,000 passed, 0 failed
Stress:              89 passed, 0 failed
Benchmarks:          45 (no regressions)

Coverage:          94.5% (target: 95%)
  Parser:          98.2%
  Type Checker:    96.1%
  Code Gen:        91.3%
  Runtime:         93.8%

Performance vs v1.0.0:
  Compilation:     +12% faster
  Execution:       +5% faster
  Memory:          -8% usage

Time: 4m 32s
```

## Testing Philosophy

### Principles

- **Test at every level**: Unit to system
- **Automate everything**: No manual testing
- **Fast feedback**: Quick tests run first
- **Prevent regressions**: Every bug gets a test
- **Measure everything**: Performance, memory, coverage

### Coverage Goals

- **Unit tests**: 95% line coverage
- **Integration**: All features tested
- **Property**: 10,000+ cases per property
- **Fuzzing**: 24/7 continuous
- **Benchmarks**: Track every commit

### Quality Gates

- All tests must pass
- No performance regressions >5%
- Coverage must not decrease
- No new fuzzing crashes
- All platforms must pass

## Summary

The Goo testing strategy ensures:

- **Correctness**: Through comprehensive testing
- **Safety**: Through property and fuzz testing
- **Performance**: Through continuous benchmarking
- **Reliability**: Through stress testing
- **Compatibility**: Through regression testing

This multi-layered approach catches bugs early, prevents regressions, and ensures Goo meets its promises of safety and performance.
