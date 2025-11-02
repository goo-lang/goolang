# Test-Driven Development Strategy for Goo Language

## Overview

This document outlines the Test-Driven Development (TDD) approach for completing the Goo programming language. We follow the Red-Green-Refactor cycle:

1. **RED**: Write a failing test that defines desired functionality
2. **GREEN**: Write minimal code to make the test pass
3. **REFACTOR**: Improve the code while keeping tests passing

## Testing Pyramid

```
        /\
       /  \      E2E Tests (5%)
      /____\     - Complete program compilation
     /      \    - Integration with runtime
    /        \
   /__________\  Integration Tests (25%)
  /            \ - Parser + Type Checker
 /              \- Type Checker + Codegen
/________________\ Unit Tests (70%)
                   - Individual components
                   - Functions and modules
```

## Test Categories

### 1. Unit Tests (70% of tests)
**Location**: `tests/unit/`

#### Structure:
```
tests/unit/
├── lexer/          # Tokenization tests
├── parser/         # AST generation tests
├── types/          # Type system tests
├── codegen/        # LLVM IR generation tests
├── runtime/        # Runtime function tests
├── memory/         # Memory safety tests
├── concurrency/    # Concurrency primitives
└── errors/         # Error handling tests
```

#### Test Template:
```c
// tests/unit/feature/feature_test.c
#include "test_framework.h"
#include "feature.h"

void test_feature_basic_case(void) {
    // Arrange
    setup_test_data();

    // Act
    result = feature_function(input);

    // Assert
    ASSERT_EQUAL(expected, result);
}

void test_feature_edge_case(void) {
    // Test edge cases
}

void test_feature_error_case(void) {
    // Test error conditions
}

int main(void) {
    RUN_TEST(test_feature_basic_case);
    RUN_TEST(test_feature_edge_case);
    RUN_TEST(test_feature_error_case);
    return TEST_REPORT();
}
```

### 2. Integration Tests (25% of tests)
**Location**: `tests/integration/`

Test multiple components working together:
- Lexer → Parser → AST
- Parser → Type Checker
- Type Checker → Code Generator
- Code Generator → Runtime

#### Example:
```c
// tests/integration/compile_pipeline_test.c
void test_complete_compilation_pipeline(void) {
    // Arrange
    const char* source = "func main() { fmt.Println(\"Hello\") }";

    // Act
    tokens = lex(source);
    ast = parse(tokens);
    typed_ast = type_check(ast);
    llvm_ir = codegen(typed_ast);
    executable = compile(llvm_ir);

    // Assert
    ASSERT_NOT_NULL(executable);
    ASSERT_EQUAL(0, run(executable));
}
```

### 3. End-to-End Tests (5% of tests)
**Location**: `tests/e2e/`

Complete programs that test full language features:
```goo
// tests/e2e/error_handling.goo
package main

import "fmt"

func divide(a, b int) !int {
    if b == 0 {
        return error("division by zero")
    }
    return a / b
}

func main() {
    result := divide(10, 2) catch |err| {
        fmt.Printf("Error: %s\n", err)
        return
    }
    fmt.Printf("Result: %d\n", result)
}
```

## TDD Workflow

### Phase 1: Write Tests First

For each feature/task:

1. **Create test file** based on feature specification
2. **Write failing tests** that define expected behavior
3. **Run tests** - verify they fail for the right reason
4. **Implement minimal code** to pass tests
5. **Run tests** - verify they pass
6. **Refactor** while keeping tests green

### Phase 2: Feature-by-Feature Completion

#### Example: Compile-time Execution (Task #10)

**Step 1: Write Tests**
```c
// tests/unit/comptime/comptime_eval_test.c
void test_comptime_constant_evaluation(void) {
    const char* source = "comptime const x = 2 + 3;";
    AST* ast = parse(source);

    Value result = evaluate_comptime(ast);

    ASSERT_EQUAL(5, result.int_value);
}

void test_comptime_function_execution(void) {
    const char* source =
        "comptime func factorial(n: int) int {\n"
        "    if n <= 1 { return 1 }\n"
        "    return n * factorial(n - 1)\n"
        "}\n"
        "const x = comptime factorial(5);";

    AST* ast = parse(source);
    Value result = evaluate_comptime(ast);

    ASSERT_EQUAL(120, result.int_value);
}
```

**Step 2: Implement Feature**
```c
// src/comptime/comptime_eval.c
Value evaluate_comptime(AST* ast) {
    // Minimal implementation to pass tests
    // ...
}
```

**Step 3: Verify**
```bash
make test-comptime
# Should show GREEN (all tests pass)
```

## Test Organization by Task

### Task #21: Fearless Concurrency

```
tests/unit/concurrency/
├── actor_system_test.c          # Actor spawning, messaging
├── shared_variables_test.c      # Automatic synchronization
├── structured_concurrency_test.c # parallel {} blocks
├── advanced_channels_test.c     # Load-balanced channels
└── deadlock_prevention_test.c   # Deadlock detection

tests/integration/concurrency/
├── actor_communication_test.c   # Multiple actors
├── concurrent_data_structures_test.c
└── parallel_algorithms_test.c

tests/e2e/concurrency/
├── actor_chat_system.goo        # Real-world actor usage
├── parallel_data_processing.goo # Structured concurrency
└── distributed_system.goo       # Advanced channels
```

### Task #10: Compile-time Execution

```
tests/unit/comptime/
├── constant_evaluation_test.c   # Basic comptime
├── function_execution_test.c    # comptime functions
├── type_generation_test.c       # comptime types
└── code_generation_test.c       # comptime code gen

tests/integration/comptime/
├── comptime_pipeline_test.c     # Full comptime evaluation
└── comptime_errors_test.c       # Error handling

tests/e2e/comptime/
├── generic_data_structures.goo  # comptime generics
├── compile_time_config.goo      # Configuration
└── meta_programming.goo         # Advanced metaprogramming
```

## Continuous Testing

### Automated Test Runner

Create `scripts/tdd_watch.sh`:
```bash
#!/bin/bash
# Watch for file changes and run tests automatically

while true; do
    inotifywait -r -e modify,create src/ include/ tests/ 2>/dev/null
    clear
    echo "🔄 Changes detected, running tests..."
    make test-all
    echo "✅ Tests complete. Watching for changes..."
done
```

### Test Commands

```makefile
# Makefile additions
.PHONY: test-all test-unit test-integration test-e2e test-watch

test-all: test-unit test-integration test-e2e

test-unit:
	@echo "Running unit tests..."
	@find tests/unit -name "*_test.c" -exec {} \;

test-integration:
	@echo "Running integration tests..."
	@find tests/integration -name "*_test.c" -exec {} \;

test-e2e:
	@echo "Running end-to-end tests..."
	@for f in tests/e2e/*.goo; do ./bin/goo $$f || exit 1; done

test-watch:
	@./scripts/tdd_watch.sh

test-coverage:
	@echo "Generating coverage report..."
	@lcov --capture --directory build --output-file coverage.info
	@genhtml coverage.info --output-directory coverage/
```

## Test Metrics

### Coverage Targets
- **Line Coverage**: ≥ 80%
- **Branch Coverage**: ≥ 75%
- **Function Coverage**: ≥ 85%

### Quality Gates
✅ All tests must pass before merging
✅ No decrease in coverage
✅ No new compiler warnings
✅ All error cases tested

## Test Data Management

### Fixtures
```
tests/fixtures/
├── valid_programs/     # Correct Goo programs
├── invalid_programs/   # Programs with errors
├── edge_cases/         # Boundary conditions
└── performance/        # Large programs for benchmarking
```

### Example Fixture:
```goo
// tests/fixtures/valid_programs/error_union_basic.goo
package main

func divide(a, b int) !int {
    if b == 0 {
        return error("division by zero")
    }
    return a / b
}

func main() {
    result := divide(10, 2)!
    fmt.Printf("%d\n", result)
}
```

## Performance Testing

### Benchmarking Template:
```c
// tests/performance/lexer_benchmark.c
#include "benchmark.h"
#include "lexer.h"

void benchmark_lexer_large_file(void) {
    char* source = load_file("tests/fixtures/performance/large.goo");

    BENCHMARK_START();
    for (int i = 0; i < 1000; i++) {
        Lexer* lexer = lexer_new(source);
        while (lexer_next_token(lexer).type != TOKEN_EOF);
        lexer_free(lexer);
    }
    BENCHMARK_END("Lexer throughput");
}
```

## Error Testing

Every feature must test:
1. ✅ **Happy path** - correct usage
2. ✅ **Edge cases** - boundary conditions
3. ✅ **Error cases** - invalid input
4. ✅ **Recovery** - error handling works

### Example:
```c
// Test happy path
void test_nullable_type_with_value(void) {
    Type* nullable = type_nullable(type_int());
    ASSERT_TRUE(type_is_nullable(nullable));
}

// Test edge case
void test_nullable_of_nullable(void) {
    Type* double_nullable = type_nullable(type_nullable(type_int()));
    // Should this be allowed? Test the decision.
}

// Test error case
void test_nullable_invalid_dereference(void) {
    // Attempting to dereference without check should fail
    ASSERT_ERROR(compile("var x: ?int = nil; var y = x"));
}
```

## Documentation-Driven Tests

For each test, include:
```c
/**
 * Test: Compile-time constant evaluation
 *
 * Feature: Task #10 - Compile-time Execution
 * Spec: comptime keyword evaluates expressions at compile time
 *
 * Given: A comptime expression "2 + 3"
 * When: The compiler evaluates it
 * Then: The result is the constant value 5
 */
void test_comptime_constant_evaluation(void) {
    // ...
}
```

## TDD Checklist for Each Task

- [ ] Read task specification
- [ ] Create test file structure
- [ ] Write tests for happy path
- [ ] Write tests for edge cases
- [ ] Write tests for error cases
- [ ] Run tests (should fail - RED)
- [ ] Implement minimal code
- [ ] Run tests (should pass - GREEN)
- [ ] Refactor for clarity
- [ ] Run tests (should still pass)
- [ ] Document implementation
- [ ] Update task status
- [ ] Commit with clear message

## Success Criteria

A task is complete when:
1. ✅ All tests pass
2. ✅ Code coverage meets targets (≥80%)
3. ✅ No compiler warnings
4. ✅ Documentation is complete
5. ✅ Examples work end-to-end
6. ✅ Performance benchmarks are acceptable

---

**Let's build Goo with confidence through comprehensive testing!** 🚀
