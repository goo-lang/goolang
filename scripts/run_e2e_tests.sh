#!/bin/bash
# End-to-End Test Runner for Goo Language
# This script runs all E2E tests and reports results

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0

# Test directory
TEST_DIR="tests/e2e"
COMPILER="./bin/goo"
ANALYZER="./bin/goo-analyzer"

# Output directory for test results
OUTPUT_DIR="test_results"
mkdir -p "$OUTPUT_DIR"

echo -e "${BLUE}================================${NC}"
echo -e "${BLUE}  Goo E2E Test Suite${NC}"
echo -e "${BLUE}================================${NC}"
echo ""

# Check if analyzer exists (fallback to analyzer if full compiler not available)
if [ ! -f "$COMPILER" ] && [ ! -f "$ANALYZER" ]; then
    echo -e "${RED}✗ Neither compiler nor analyzer found${NC}"
    echo -e "${YELLOW}  Run 'make analyzer' to build the analyzer${NC}"
    exit 1
fi

# Determine which tool to use
if [ -f "$COMPILER" ]; then
    TOOL="$COMPILER"
    TOOL_NAME="Compiler"
elif [ -f "$ANALYZER" ]; then
    TOOL="$ANALYZER"
    TOOL_NAME="Analyzer (Lexer)"
fi

echo -e "${YELLOW}Using: $TOOL_NAME${NC}"
echo ""

# Function to run a single test
run_test() {
    local test_file=$1
    local test_name=$(basename "$test_file" .goo)

    TOTAL=$((TOTAL + 1))

    echo -n "  Testing $test_name ... "

    # Run the compiler on the test file
    if $COMPILER "$test_file" > "$OUTPUT_DIR/$test_name.out" 2>&1; then
        echo -e "${GREEN}✓ PASS${NC}"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo -e "${RED}✗ FAIL${NC}"
        FAILED=$((FAILED + 1))
        echo -e "${YELLOW}    See $OUTPUT_DIR/$test_name.out for details${NC}"
        return 1
    fi
}

# Function to run tests with lexer only (current capability)
run_lexer_test() {
    local test_file=$1
    local test_name=$(basename "$test_file" .goo)

    TOTAL=$((TOTAL + 1))

    echo -n "  Lexing $test_name ... "

    # Run the analyzer (lexer) on the test file
    if $TOOL "$test_file" > "$OUTPUT_DIR/$test_name.out" 2>&1; then
        echo -e "${GREEN}✓ PASS${NC}"
        PASSED=$((PASSED + 1))

        # Count tokens
        token_count=$(grep -c "Token:" "$OUTPUT_DIR/$test_name.out" || true)
        if [ $token_count -gt 0 ]; then
            echo -e "${BLUE}    → $token_count tokens analyzed${NC}"
        fi
        return 0
    else
        echo -e "${RED}✗ FAIL${NC}"
        FAILED=$((FAILED + 1))
        echo -e "${YELLOW}    See $OUTPUT_DIR/$test_name.out for details${NC}"
        return 1
    fi
}

# Run all E2E tests
echo -e "${BLUE}Running E2E tests from $TEST_DIR${NC}"
echo ""

for test_file in "$TEST_DIR"/*.goo; do
    if [ -f "$test_file" ]; then
        # Currently using lexer-only tests
        # TODO: Switch to run_test when full compiler is ready
        run_lexer_test "$test_file"
    fi
done

echo ""
echo -e "${BLUE}================================${NC}"
echo -e "${BLUE}  Test Results${NC}"
echo -e "${BLUE}================================${NC}"
echo -e "  Total:   $TOTAL"
echo -e "  ${GREEN}Passed:  $PASSED${NC}"
echo -e "  ${RED}Failed:  $FAILED${NC}"
echo -e "  ${YELLOW}Skipped: $SKIPPED${NC}"
echo ""

# Calculate percentage
if [ $TOTAL -gt 0 ]; then
    PERCENT=$((PASSED * 100 / TOTAL))
    echo -e "  Pass rate: ${GREEN}$PERCENT%${NC}"
fi

echo ""

# Exit with failure if any tests failed
if [ $FAILED -gt 0 ]; then
    echo -e "${RED}✗ Some tests failed${NC}"
    exit 1
else
    echo -e "${GREEN}✓ All tests passed!${NC}"
    exit 0
fi
