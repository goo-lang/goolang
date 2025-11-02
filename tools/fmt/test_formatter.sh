#!/bin/bash

# Test script for the Goo formatter
set -e

FORMATTER="./goo-fmt"
TEST_DIR="test_files"
TEMP_DIR="/tmp/goo_fmt_test"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

function log_info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

function log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
}

function log_failure() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

function run_test() {
    local test_name="$1"
    local input_file="$2"
    local expected_file="$3"
    local formatter_args="$4"
    
    ((TESTS_RUN++))
    log_info "Running test: $test_name"
    
    # Create temporary output file
    local output_file="$TEMP_DIR/output.goo"
    
    # Run formatter
    if $FORMATTER $formatter_args "$input_file" > "$output_file" 2>/dev/null; then
        # Compare output with expected
        if diff -q "$output_file" "$expected_file" >/dev/null 2>&1; then
            log_success "$test_name"
        else
            log_failure "$test_name - Output differs from expected"
            echo "Expected:"
            cat "$expected_file"
            echo -e "\nActual:"
            cat "$output_file"
            echo -e "\nDiff:"
            diff "$expected_file" "$output_file" || true
        fi
    else
        log_failure "$test_name - Formatter execution failed"
    fi
}

function create_test_files() {
    mkdir -p "$TEST_DIR"
    
    # Test 1: Basic formatting
    cat > "$TEST_DIR/basic_input.goo" << 'EOF'
package main
import "fmt"
func main(){fmt.Println("Hello, World!")}
EOF
    
    cat > "$TEST_DIR/basic_expected.goo" << 'EOF'
package main

import "fmt"

func main() {
    fmt.Println("Hello, World!")
}
EOF
    
    # Test 2: Function with parameters
    cat > "$TEST_DIR/function_input.goo" << 'EOF'
func add(a:int,b:int)->int{return a+b}
func multiply(x:int,y:int)->int{
return x*y
}
EOF
    
    cat > "$TEST_DIR/function_expected.goo" << 'EOF'
func add(a: int, b: int) -> int {
    return a + b
}

func multiply(x: int, y: int) -> int {
    return x * y
}
EOF
    
    # Test 3: Struct definition
    cat > "$TEST_DIR/struct_input.goo" << 'EOF'
type Person struct{
name:string
age:int
email:?string
}
EOF
    
    cat > "$TEST_DIR/struct_expected.goo" << 'EOF'
type Person struct {
    name: string
    age: int
    email: ?string
}
EOF
    
    # Test 4: Complex expressions
    cat > "$TEST_DIR/expressions_input.goo" << 'EOF'
func calculate()->int{
x:=10
y:=20
result:=x+y*2-(x/3)
return result
}
EOF
    
    cat > "$TEST_DIR/expressions_expected.goo" << 'EOF'
func calculate() -> int {
    x := 10
    y := 20
    result := x + y * 2 - (x / 3)
    return result
}
EOF
    
    # Test 5: Error unions and nullable types
    cat > "$TEST_DIR/types_input.goo" << 'EOF'
func riskyOperation()->!int{
value:=?int(42)
if value!=nil{
return *value
}
return ErrorNotFound
}
EOF
    
    cat > "$TEST_DIR/types_expected.goo" << 'EOF'
func riskyOperation() -> !int {
    value := ?int(42)
    if value != nil {
        return *value
    }
    return ErrorNotFound
}
EOF
    
    # Test 6: Comments preservation
    cat > "$TEST_DIR/comments_input.goo" << 'EOF'
// Main package
package main

/* Multi-line
   comment */
func main(){
// Print hello
fmt.Println("Hello")/* inline comment */
}
EOF
    
    cat > "$TEST_DIR/comments_expected.goo" << 'EOF'
// Main package
package main

/* Multi-line
   comment */
func main() {
    // Print hello
    fmt.Println("Hello") /* inline comment */
}
EOF
}

function cleanup() {
    rm -rf "$TEMP_DIR"
    rm -rf "$TEST_DIR"
}

function main() {
    log_info "Starting Goo formatter tests"
    
    # Check if formatter exists
    if [ ! -f "$FORMATTER" ]; then
        log_failure "Formatter not found: $FORMATTER"
        log_info "Please run 'make' to build the formatter first"
        exit 1
    fi
    
    # Create temporary directory
    mkdir -p "$TEMP_DIR"
    
    # Create test files
    create_test_files
    
    # Run tests
    run_test "Basic formatting" "$TEST_DIR/basic_input.goo" "$TEST_DIR/basic_expected.goo" ""
    run_test "Function formatting" "$TEST_DIR/function_input.goo" "$TEST_DIR/function_expected.goo" ""
    run_test "Struct formatting" "$TEST_DIR/struct_input.goo" "$TEST_DIR/struct_expected.goo" ""
    run_test "Expression formatting" "$TEST_DIR/expressions_input.goo" "$TEST_DIR/expressions_expected.goo" ""
    run_test "Type formatting" "$TEST_DIR/types_input.goo" "$TEST_DIR/types_expected.goo" ""
    run_test "Comment preservation" "$TEST_DIR/comments_input.goo" "$TEST_DIR/comments_expected.goo" ""
    
    # Test configuration options
    log_info "Testing configuration options..."
    
    # Test with tabs
    if $FORMATTER --tabs "$TEST_DIR/basic_input.goo" | grep -q $'\t'; then
        log_success "Tab indentation option"
        ((TESTS_RUN++))
    else
        log_failure "Tab indentation option"
        ((TESTS_RUN++))
    fi
    
    # Test with different indent size
    output=$($FORMATTER --indent 2 "$TEST_DIR/basic_input.goo")
    if echo "$output" | grep -q "  fmt.Println"; then
        log_success "Custom indent size option"
        ((TESTS_RUN++))
    else
        log_failure "Custom indent size option"
        ((TESTS_RUN++))
    fi
    
    # Test check mode
    if $FORMATTER -c "$TEST_DIR/basic_input.goo" >/dev/null 2>&1; then
        exit_code=$?
        if [ $exit_code -eq 1 ]; then
            log_success "Check mode (detects unformatted file)"
            ((TESTS_RUN++))
        else
            log_failure "Check mode (should detect unformatted file)"
            ((TESTS_RUN++))
        fi
    else
        log_failure "Check mode execution failed"
        ((TESTS_RUN++))
    fi
    
    # Test write mode
    cp "$TEST_DIR/basic_input.goo" "$TEMP_DIR/write_test.goo"
    if $FORMATTER -w "$TEMP_DIR/write_test.goo" >/dev/null 2>&1; then
        if grep -q "func main() {" "$TEMP_DIR/write_test.goo"; then
            log_success "Write mode (modifies file in place)"
            ((TESTS_RUN++))
        else
            log_failure "Write mode (file not properly formatted)"
            ((TESTS_RUN++))
        fi
    else
        log_failure "Write mode execution failed"
        ((TESTS_RUN++))
    fi
    
    # Test error handling
    if ! $FORMATTER "/nonexistent/file.goo" >/dev/null 2>&1; then
        log_success "Error handling (nonexistent file)"
        ((TESTS_RUN++))
    else
        log_failure "Error handling (should fail on nonexistent file)"
        ((TESTS_RUN++))
    fi
    
    # Cleanup
    cleanup
    
    # Print summary
    echo
    log_info "Test Summary:"
    echo "  Tests run: $TESTS_RUN"
    echo "  Passed: $TESTS_PASSED"
    echo "  Failed: $TESTS_FAILED"
    
    if [ $TESTS_FAILED -eq 0 ]; then
        log_success "All tests passed!"
        exit 0
    else
        log_failure "$TESTS_FAILED test(s) failed"
        exit 1
    fi
}

# Set trap to cleanup on exit
trap cleanup EXIT

# Run main function
main "$@"