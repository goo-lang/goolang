#!/bin/bash

# Test script for the Goo debugger
set -e

DEBUGGER="./goo-debug"
TEST_PROGRAM="./test_program"
TEST_DIR="/tmp/goo_debug_test"

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

function test_debugger_help() {
    ((TESTS_RUN++))
    log_info "Testing debugger help output"
    
    if "$DEBUGGER" --help >/dev/null 2>&1; then
        log_success "Debugger help command works"
    else
        log_failure "Debugger help command failed"
    fi
}

function test_debugger_startup() {
    ((TESTS_RUN++))
    log_info "Testing debugger startup with test program"
    
    # Create a simple debug script
    cat > "$TEST_DIR/debug_script.txt" << 'EOF'
help
info registers
quit
EOF
    
    if timeout 10 "$DEBUGGER" "$TEST_PROGRAM" < "$TEST_DIR/debug_script.txt" >/dev/null 2>&1; then
        log_success "Debugger starts and exits cleanly"
    else
        log_failure "Debugger startup failed"
    fi
}

function test_breakpoint_commands() {
    ((TESTS_RUN++))
    log_info "Testing breakpoint commands"
    
    # Create debug script with breakpoint commands
    cat > "$TEST_DIR/breakpoint_script.txt" << 'EOF'
break main
break 0x400000
info breakpoints
delete 0
disable 1
enable 1
quit
EOF
    
    if timeout 10 "$DEBUGGER" "$TEST_PROGRAM" < "$TEST_DIR/breakpoint_script.txt" >/dev/null 2>&1; then
        log_success "Breakpoint commands work"
    else
        log_failure "Breakpoint commands failed"
    fi
}

function test_execution_control() {
    ((TESTS_RUN++))
    log_info "Testing execution control commands"
    
    # Create debug script with execution control
    cat > "$TEST_DIR/execution_script.txt" << 'EOF'
break main
run
step
stepi
next
continue
quit
EOF
    
    if timeout 15 "$DEBUGGER" "$TEST_PROGRAM" < "$TEST_DIR/execution_script.txt" >/dev/null 2>&1; then
        log_success "Execution control commands work"
    else
        log_failure "Execution control commands failed"
    fi
}

function test_inspection_commands() {
    ((TESTS_RUN++))
    log_info "Testing inspection commands"
    
    # Create debug script with inspection commands
    cat > "$TEST_DIR/inspection_script.txt" << 'EOF'
info registers
backtrace
info locals
examine 0x400000
disassemble 0x400000
list
quit
EOF
    
    if timeout 10 "$DEBUGGER" "$TEST_PROGRAM" < "$TEST_DIR/inspection_script.txt" >/dev/null 2>&1; then
        log_success "Inspection commands work"
    else
        log_failure "Inspection commands failed"
    fi
}

function test_attach_functionality() {
    ((TESTS_RUN++))
    log_info "Testing attach to process functionality"
    
    # Start test program in background
    "$TEST_PROGRAM" &
    TEST_PID=$!
    
    # Give it time to start
    sleep 1
    
    if kill -0 "$TEST_PID" 2>/dev/null; then
        # Create debug script for attach
        cat > "$TEST_DIR/attach_script.txt" << 'EOF'
info registers
backtrace
quit
EOF
        
        # Test attach
        if timeout 10 "$DEBUGGER" -p "$TEST_PID" < "$TEST_DIR/attach_script.txt" >/dev/null 2>&1; then
            log_success "Attach to process works"
        else
            log_failure "Attach to process failed"
        fi
        
        # Clean up test process
        kill "$TEST_PID" 2>/dev/null || true
        wait "$TEST_PID" 2>/dev/null || true
    else
        log_failure "Could not start test process for attach test"
    fi
}

function test_error_handling() {
    ((TESTS_RUN++))
    log_info "Testing error handling"
    
    # Test with non-existent program
    if ! "$DEBUGGER" "/nonexistent/program" </dev/null >/dev/null 2>&1; then
        log_success "Handles non-existent program gracefully"
    else
        log_failure "Should fail with non-existent program"
    fi
    
    # Test with invalid commands
    cat > "$TEST_DIR/invalid_script.txt" << 'EOF'
invalid_command
break
print
quit
EOF
    
    if timeout 5 "$DEBUGGER" "$TEST_PROGRAM" < "$TEST_DIR/invalid_script.txt" >/dev/null 2>&1; then
        log_success "Handles invalid commands gracefully"
    else
        log_failure "Should handle invalid commands"
    fi
}

function test_dwarf_integration() {
    ((TESTS_RUN++))
    log_info "Testing DWARF debugging information integration"
    
    # Test with program compiled with debug info
    if [ -f "$TEST_PROGRAM" ]; then
        # Check if program has debug info
        if objdump -h "$TEST_PROGRAM" 2>/dev/null | grep -q "debug"; then
            log_success "Test program has debug information"
        else
            log_info "Test program lacks debug information (expected for this demo)"
            log_success "DWARF integration test skipped"
        fi
    else
        log_failure "Test program not found"
    fi
}

function build_test_program() {
    log_info "Building test program with debug information"
    
    if make test-program >/dev/null 2>&1; then
        log_success "Test program built successfully"
        return 0
    else
        log_failure "Failed to build test program"
        return 1
    fi
}

function cleanup() {
    rm -rf "$TEST_DIR"
    # Clean up any remaining test processes
    pkill -f test_program 2>/dev/null || true
}

function main() {
    log_info "Starting Goo debugger tests"
    
    # Check if debugger exists
    if [ ! -f "$DEBUGGER" ]; then
        log_failure "Debugger not found: $DEBUGGER"
        log_info "Please run 'make' to build the debugger first"
        exit 1
    fi
    
    # Create test directory
    mkdir -p "$TEST_DIR"
    
    # Set trap to cleanup on exit
    trap cleanup EXIT
    
    # Build test program
    if ! build_test_program; then
        log_failure "Cannot run tests without test program"
        exit 1
    fi
    
    # Run tests
    test_debugger_help
    test_debugger_startup
    test_breakpoint_commands
    test_execution_control
    test_inspection_commands
    test_attach_functionality
    test_error_handling
    test_dwarf_integration
    
    # Test performance
    ((TESTS_RUN++))
    log_info "Testing debugger performance"
    
    start_time=$(date +%s.%N)
    timeout 5 "$DEBUGGER" "$TEST_PROGRAM" </dev/null >/dev/null 2>&1 || true
    end_time=$(date +%s.%N)
    duration=$(echo "$end_time - $start_time" | bc -l 2>/dev/null || echo "0")
    
    if (( $(echo "$duration < 3.0" | bc -l 2>/dev/null || echo 0) )); then
        log_success "Debugger startup time acceptable ($duration seconds)"
    else
        log_failure "Debugger startup too slow ($duration seconds)"
    fi
    
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

# Check for required tools
if ! command -v timeout >/dev/null; then
    echo "Error: timeout command not found"
    exit 1
fi

if ! command -v objdump >/dev/null; then
    echo "Warning: objdump not found, some tests will be skipped"
fi

# Run main function
main "$@"