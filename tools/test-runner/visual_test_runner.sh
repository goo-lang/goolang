#!/bin/bash

# Visual Test Runner for Goo Projects
# Provides comprehensive testing with visual reports and real-time feedback

set -e

# Configuration
SCRIPT_DIR="$(dirname "$0")"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
RESULTS_DIR="$PROJECT_ROOT/test-results"
REPORTS_DIR="$RESULTS_DIR/reports"

# Colors and formatting
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# Unicode symbols
CHECK="✅"
CROSS="❌"
WARNING="⚠️"
INFO="ℹ️"
ROCKET="🚀"
GEAR="⚙️"
CHART="📊"
CLOCK="⏱️"
FIRE="🔥"

# Test configuration
VERBOSE=false
WATCH_MODE=false
COVERAGE=false
PARALLEL=false
OUTPUT_FORMAT="console"
FILTER=""
TIMEOUT=30

# Statistics
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0
START_TIME=""
END_TIME=""

# Helper functions
log_info() {
    echo -e "${BLUE}${INFO} $1${NC}"
}

log_success() {
    echo -e "${GREEN}${CHECK} $1${NC}"
}

log_warning() {
    echo -e "${YELLOW}${WARNING} $1${NC}"
}

log_error() {
    echo -e "${RED}${CROSS} $1${NC}"
}

log_header() {
    echo -e "${CYAN}${BOLD}${ROCKET} $1${NC}"
}

log_section() {
    echo -e "${MAGENTA}${BOLD}═══ $1 ═══${NC}"
}

# Show usage
show_usage() {
    echo -e "${BOLD}Goo Visual Test Runner${NC}"
    echo ""
    echo "Usage: $0 [options] [test-pattern]"
    echo ""
    echo "Options:"
    echo "  -v, --verbose       Verbose output"
    echo "  -w, --watch         Watch mode (re-run on file changes)"
    echo "  -c, --coverage      Generate coverage reports"
    echo "  -p, --parallel      Run tests in parallel"
    echo "  -f, --filter PATTERN Filter tests by pattern"
    echo "  -t, --timeout SECONDS Test timeout (default: 30)"
    echo "  -o, --output FORMAT Output format (console|html|json|junit)"
    echo "  -h, --help          Show this help"
    echo ""
    echo "Examples:"
    echo "  $0                          # Run all tests"
    echo "  $0 -v -c                    # Verbose mode with coverage"
    echo "  $0 -w tests/unit            # Watch unit tests"
    echo "  $0 -o html -f integration   # HTML report for integration tests"
}

# Initialize test environment
init_test_env() {
    log_header "Initializing Test Environment"
    
    # Create directories
    mkdir -p "$RESULTS_DIR"
    mkdir -p "$REPORTS_DIR"
    mkdir -p "$RESULTS_DIR/coverage"
    mkdir -p "$RESULTS_DIR/logs"
    
    # Clean previous results
    rm -f "$RESULTS_DIR"/*.xml
    rm -f "$RESULTS_DIR"/*.json
    rm -f "$REPORTS_DIR"/*.html
    
    START_TIME=$(date +%s)
    
    log_success "Test environment initialized"
}

# Discover test files
discover_tests() {
    local pattern="${1:-}"
    local test_files=()
    
    log_info "Discovering test files..."
    
    # Find test files
    while IFS= read -r -d '' file; do
        if [[ -z "$FILTER" ]] || [[ "$file" == *"$FILTER"* ]]; then
            test_files+=("$file")
        fi
    done < <(find "$PROJECT_ROOT" -name "*test*.goo" -type f -print0 2>/dev/null)
    
    # Find shell test files
    while IFS= read -r -d '' file; do
        if [[ -z "$FILTER" ]] || [[ "$file" == *"$FILTER"* ]]; then
            test_files+=("$file")
        fi
    done < <(find "$PROJECT_ROOT" -name "*test*.sh" -type f -print0 2>/dev/null)
    
    TOTAL_TESTS=${#test_files[@]}
    
    if [ $TOTAL_TESTS -eq 0 ]; then
        log_warning "No test files found"
        return 1
    fi
    
    log_success "Found $TOTAL_TESTS test files"
    
    if [ "$VERBOSE" = true ]; then
        for file in "${test_files[@]}"; do
            echo "  • $(basename "$file")"
        done
    fi
    
    # Export for other functions
    printf '%s\n' "${test_files[@]}" > "$RESULTS_DIR/test_files.txt"
}

# Run a single test file
run_test_file() {
    local test_file="$1"
    local test_name=$(basename "$test_file" .goo)
    local test_name=$(basename "$test_name" .sh)
    local test_log="$RESULTS_DIR/logs/${test_name}.log"
    local test_result="$RESULTS_DIR/${test_name}.result"
    
    echo "running" > "$test_result"
    
    log_info "Running: $test_name"
    
    local start_time=$(date +%s.%3N)
    local exit_code=0
    
    # Run the test based on file type
    if [[ "$test_file" == *.goo ]]; then
        # Goo test file
        if command -v goo >/dev/null 2>&1; then
            timeout "$TIMEOUT" goo test "$test_file" >"$test_log" 2>&1 || exit_code=$?
        else
            # Fallback: try to compile and run
            timeout "$TIMEOUT" make test >"$test_log" 2>&1 || exit_code=$?
        fi
    elif [[ "$test_file" == *.sh ]]; then
        # Shell test file
        timeout "$TIMEOUT" bash "$test_file" >"$test_log" 2>&1 || exit_code=$?
    fi
    
    local end_time=$(date +%s.%3N)
    local duration=$(echo "$end_time - $start_time" | bc -l 2>/dev/null || echo "0")
    
    # Record result
    if [ $exit_code -eq 0 ]; then
        echo "passed:$duration" > "$test_result"
        log_success "$test_name (${duration}s)"
        ((PASSED_TESTS++))
    elif [ $exit_code -eq 124 ]; then
        echo "timeout:$duration" > "$test_result"
        log_error "$test_name (timeout after ${TIMEOUT}s)"
        ((FAILED_TESTS++))
    else
        echo "failed:$duration:$exit_code" > "$test_result"
        log_error "$test_name (${duration}s, exit code: $exit_code)"
        ((FAILED_TESTS++))
        
        if [ "$VERBOSE" = true ]; then
            echo -e "${DIM}$(head -10 "$test_log")${NC}"
        fi
    fi
}

# Run all tests
run_tests() {
    log_section "Running Tests"
    
    if [ ! -f "$RESULTS_DIR/test_files.txt" ]; then
        log_error "No test files discovered"
        return 1
    fi
    
    local test_files=()
    while IFS= read -r line; do
        test_files+=("$line")
    done < "$RESULTS_DIR/test_files.txt"
    
    # Progress tracking
    local current=0
    local total=${#test_files[@]}
    
    # Run tests
    if [ "$PARALLEL" = true ] && [ $total -gt 1 ]; then
        log_info "Running $total tests in parallel..."
        
        # Parallel execution
        for test_file in "${test_files[@]}"; do
            run_test_file "$test_file" &
            
            # Limit concurrent jobs
            if (( $(jobs -r | wc -l) >= 4 )); then
                wait -n
            fi
        done
        
        # Wait for all jobs
        wait
    else
        log_info "Running $total tests sequentially..."
        
        # Sequential execution with progress
        for test_file in "${test_files[@]}"; do
            ((current++))
            echo -ne "\r${CYAN}Progress: $current/$total${NC}"
            run_test_file "$test_file"
        done
        echo ""
    fi
}

# Generate console report
generate_console_report() {
    END_TIME=$(date +%s)
    local duration=$((END_TIME - START_TIME))
    
    log_section "Test Results Summary"
    
    echo ""
    echo -e "${BOLD}${CHART} Test Statistics${NC}"
    echo "  Total:   $TOTAL_TESTS"
    echo -e "  ${GREEN}Passed:  $PASSED_TESTS${NC}"
    echo -e "  ${RED}Failed:  $FAILED_TESTS${NC}"
    echo -e "  ${YELLOW}Skipped: $SKIPPED_TESTS${NC}"
    echo "  Duration: ${duration}s"
    
    # Success rate
    if [ $TOTAL_TESTS -gt 0 ]; then
        local success_rate=$((PASSED_TESTS * 100 / TOTAL_TESTS))
        echo "  Success Rate: ${success_rate}%"
        
        # Visual progress bar
        local bar_length=40
        local filled_length=$((success_rate * bar_length / 100))
        
        echo -n "  Progress: ["
        for ((i=0; i<filled_length; i++)); do echo -n "█"; done
        for ((i=filled_length; i<bar_length; i++)); do echo -n "░"; done
        echo "] ${success_rate}%"
    fi
    
    echo ""
    
    # Show failed tests
    if [ $FAILED_TESTS -gt 0 ]; then
        echo -e "${RED}${BOLD}Failed Tests:${NC}"
        
        for result_file in "$RESULTS_DIR"/*.result; do
            if [ -f "$result_file" ]; then
                local result=$(cat "$result_file")
                if [[ "$result" == failed* ]] || [[ "$result" == timeout* ]]; then
                    local test_name=$(basename "$result_file" .result)
                    local log_file="$RESULTS_DIR/logs/${test_name}.log"
                    
                    echo "  ${CROSS} $test_name"
                    
                    if [ "$VERBOSE" = true ] && [ -f "$log_file" ]; then
                        echo -e "${DIM}$(head -5 "$log_file" | sed 's/^/    /')${NC}"
                    fi
                fi
            fi
        done
        echo ""
    fi
    
    # Overall status
    if [ $FAILED_TESTS -eq 0 ]; then
        log_success "All tests passed! ${FIRE}"
    else
        log_error "$FAILED_TESTS test(s) failed"
    fi
}

# Generate HTML report
generate_html_report() {
    local html_file="$REPORTS_DIR/test_report.html"
    
    log_info "Generating HTML report..."
    
    cat > "$html_file" << 'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Goo Test Results</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif; margin: 0; padding: 20px; background: #f5f5f5; }
        .container { max-width: 1200px; margin: 0 auto; }
        .header { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 30px; border-radius: 12px; margin-bottom: 30px; }
        .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 20px; margin-bottom: 30px; }
        .stat-card { background: white; padding: 20px; border-radius: 8px; text-align: center; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        .stat-value { font-size: 2.5em; font-weight: bold; margin-bottom: 10px; }
        .stat-label { color: #666; font-size: 0.9em; }
        .passed { color: #28a745; }
        .failed { color: #dc3545; }
        .skipped { color: #ffc107; }
        .total { color: #667eea; }
        .progress-bar { width: 100%; height: 20px; background: #e9ecef; border-radius: 10px; overflow: hidden; margin: 20px 0; }
        .progress-fill { height: 100%; background: linear-gradient(90deg, #28a745, #20c997); transition: width 0.3s ease; }
        .test-results { background: white; border-radius: 8px; padding: 20px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        .test-item { padding: 15px; border-left: 4px solid #e9ecef; margin-bottom: 10px; border-radius: 0 4px 4px 0; }
        .test-passed { border-left-color: #28a745; background: #f8fff9; }
        .test-failed { border-left-color: #dc3545; background: #fff8f8; }
        .test-timeout { border-left-color: #ffc107; background: #fffbf0; }
        .test-name { font-weight: bold; margin-bottom: 5px; }
        .test-duration { color: #666; font-size: 0.9em; }
        .test-error { background: #f8f9fa; padding: 10px; border-radius: 4px; margin-top: 10px; font-family: monospace; font-size: 0.8em; color: #dc3545; }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🚀 Goo Test Results</h1>
            <p>Generated on <span id="timestamp"></span></p>
        </div>
        
        <div class="stats">
            <div class="stat-card">
                <div class="stat-value total" id="total-tests">0</div>
                <div class="stat-label">Total Tests</div>
            </div>
            <div class="stat-card">
                <div class="stat-value passed" id="passed-tests">0</div>
                <div class="stat-label">Passed</div>
            </div>
            <div class="stat-card">
                <div class="stat-value failed" id="failed-tests">0</div>
                <div class="stat-label">Failed</div>
            </div>
            <div class="stat-card">
                <div class="stat-value skipped" id="skipped-tests">0</div>
                <div class="stat-label">Skipped</div>
            </div>
        </div>
        
        <div class="progress-bar">
            <div class="progress-fill" id="progress-fill" style="width: 0%"></div>
        </div>
        
        <div class="test-results">
            <h3>Test Details</h3>
            <div id="test-list">
                <!-- Test items will be populated by JavaScript -->
            </div>
        </div>
    </div>
    
    <script>
        // Set timestamp
        document.getElementById('timestamp').textContent = new Date().toLocaleString();
        
        // This will be populated by the shell script
        const testData = {};
    </script>
</body>
</html>
EOF

    # Populate test data
    local total_tests=0
    local passed_tests=0
    local failed_tests=0
    local skipped_tests=0
    
    # Generate JavaScript with test data
    cat >> "$html_file" << EOF
    <script>
        // Update statistics
        document.getElementById('total-tests').textContent = '$TOTAL_TESTS';
        document.getElementById('passed-tests').textContent = '$PASSED_TESTS';
        document.getElementById('failed-tests').textContent = '$FAILED_TESTS';
        document.getElementById('skipped-tests').textContent = '$SKIPPED_TESTS';
        
        // Update progress bar
        const successRate = $TOTAL_TESTS > 0 ? Math.round($PASSED_TESTS * 100 / $TOTAL_TESTS) : 0;
        document.getElementById('progress-fill').style.width = successRate + '%';
        
        // Populate test list
        const testList = document.getElementById('test-list');
        const tests = [
EOF

    # Add test results
    for result_file in "$RESULTS_DIR"/*.result; do
        if [ -f "$result_file" ]; then
            local test_name=$(basename "$result_file" .result)
            local result=$(cat "$result_file")
            local log_file="$RESULTS_DIR/logs/${test_name}.log"
            
            local status="unknown"
            local duration="0"
            local error_msg=""
            
            if [[ "$result" == passed* ]]; then
                status="passed"
                duration=$(echo "$result" | cut -d: -f2)
            elif [[ "$result" == failed* ]]; then
                status="failed"
                duration=$(echo "$result" | cut -d: -f2)
                if [ -f "$log_file" ]; then
                    error_msg=$(head -5 "$log_file" | sed 's/"/\\"/g' | tr '\n' ' ')
                fi
            elif [[ "$result" == timeout* ]]; then
                status="timeout"
                duration=$(echo "$result" | cut -d: -f2)
                error_msg="Test timed out after ${TIMEOUT} seconds"
            fi
            
            cat >> "$html_file" << EOF
            {
                name: "$test_name",
                status: "$status",
                duration: "$duration",
                error: "$error_msg"
            },
EOF
        fi
    done

    cat >> "$html_file" << 'EOF'
        ];
        
        tests.forEach(test => {
            const testDiv = document.createElement('div');
            testDiv.className = `test-item test-${test.status}`;
            
            let statusIcon = '';
            switch(test.status) {
                case 'passed': statusIcon = '✅'; break;
                case 'failed': statusIcon = '❌'; break;
                case 'timeout': statusIcon = '⏱️'; break;
                default: statusIcon = '❓';
            }
            
            testDiv.innerHTML = `
                <div class="test-name">${statusIcon} ${test.name}</div>
                <div class="test-duration">Duration: ${test.duration}s</div>
                ${test.error ? `<div class="test-error">${test.error}</div>` : ''}
            `;
            
            testList.appendChild(testDiv);
        });
    </script>
</body>
</html>
EOF

    log_success "HTML report generated: $html_file"
}

# Generate JSON report
generate_json_report() {
    local json_file="$RESULTS_DIR/test_results.json"
    
    log_info "Generating JSON report..."
    
    cat > "$json_file" << EOF
{
    "timestamp": "$(date -Iseconds)",
    "summary": {
        "total": $TOTAL_TESTS,
        "passed": $PASSED_TESTS,
        "failed": $FAILED_TESTS,
        "skipped": $SKIPPED_TESTS,
        "success_rate": $([ $TOTAL_TESTS -gt 0 ] && echo "$((PASSED_TESTS * 100 / TOTAL_TESTS))" || echo "0"),
        "duration": $((END_TIME - START_TIME))
    },
    "tests": [
EOF

    local first=true
    for result_file in "$RESULTS_DIR"/*.result; do
        if [ -f "$result_file" ]; then
            [ "$first" = false ] && echo "," >> "$json_file"
            first=false
            
            local test_name=$(basename "$result_file" .result)
            local result=$(cat "$result_file")
            local log_file="$RESULTS_DIR/logs/${test_name}.log"
            
            echo "        {" >> "$json_file"
            echo "            \"name\": \"$test_name\"," >> "$json_file"
            
            if [[ "$result" == passed* ]]; then
                local duration=$(echo "$result" | cut -d: -f2)
                echo "            \"status\": \"passed\"," >> "$json_file"
                echo "            \"duration\": $duration" >> "$json_file"
            elif [[ "$result" == failed* ]]; then
                local duration=$(echo "$result" | cut -d: -f2)
                local exit_code=$(echo "$result" | cut -d: -f3)
                echo "            \"status\": \"failed\"," >> "$json_file"
                echo "            \"duration\": $duration," >> "$json_file"
                echo "            \"exit_code\": $exit_code," >> "$json_file"
                if [ -f "$log_file" ]; then
                    local error_output=$(head -10 "$log_file" | jq -Rs .)
                    echo "            \"error\": $error_output" >> "$json_file"
                else
                    echo "            \"error\": null" >> "$json_file"
                fi
            elif [[ "$result" == timeout* ]]; then
                local duration=$(echo "$result" | cut -d: -f2)
                echo "            \"status\": \"timeout\"," >> "$json_file"
                echo "            \"duration\": $duration," >> "$json_file"
                echo "            \"timeout\": $TIMEOUT" >> "$json_file"
            fi
            
            echo "        }" >> "$json_file"
        fi
    done

    cat >> "$json_file" << EOF
    ]
}
EOF

    log_success "JSON report generated: $json_file"
}

# Watch mode implementation
watch_tests() {
    log_header "Starting Watch Mode"
    log_info "Watching for file changes..."
    
    while true; do
        # Run tests
        init_test_env
        discover_tests "$@"
        run_tests
        generate_console_report
        
        log_info "Waiting for file changes... (Ctrl+C to exit)"
        
        # Simple file watching (could be improved with inotify)
        local last_change=$(find "$PROJECT_ROOT" -name "*.goo" -o -name "*.sh" -type f -printf '%T@\n' 2>/dev/null | sort -n | tail -1)
        
        while true; do
            sleep 2
            local current_change=$(find "$PROJECT_ROOT" -name "*.goo" -o -name "*.sh" -type f -printf '%T@\n' 2>/dev/null | sort -n | tail -1)
            
            if [ "$current_change" != "$last_change" ]; then
                log_info "File changes detected, re-running tests..."
                break
            fi
        done
        
        echo ""
    done
}

# Main function
main() {
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -w|--watch)
                WATCH_MODE=true
                shift
                ;;
            -c|--coverage)
                COVERAGE=true
                shift
                ;;
            -p|--parallel)
                PARALLEL=true
                shift
                ;;
            -f|--filter)
                FILTER="$2"
                shift 2
                ;;
            -t|--timeout)
                TIMEOUT="$2"
                shift 2
                ;;
            -o|--output)
                OUTPUT_FORMAT="$2"
                shift 2
                ;;
            -h|--help)
                show_usage
                exit 0
                ;;
            *)
                # Positional argument (test pattern)
                FILTER="$1"
                shift
                ;;
        esac
    done
    
    # Check dependencies
    if ! command -v bc >/dev/null 2>&1; then
        log_warning "bc not found, duration calculations may be inaccurate"
    fi
    
    # Main execution
    if [ "$WATCH_MODE" = true ]; then
        watch_tests
    else
        init_test_env
        discover_tests
        run_tests
        
        # Generate reports
        case $OUTPUT_FORMAT in
            console)
                generate_console_report
                ;;
            html)
                generate_html_report
                ;;
            json)
                generate_json_report
                ;;
            junit)
                # TODO: Implement JUnit XML format
                log_warning "JUnit format not yet implemented, using console"
                generate_console_report
                ;;
            *)
                log_warning "Unknown output format: $OUTPUT_FORMAT, using console"
                generate_console_report
                ;;
        esac
        
        # Exit with appropriate code
        if [ $FAILED_TESTS -gt 0 ]; then
            exit 1
        else
            exit 0
        fi
    fi
}

# Handle Ctrl+C gracefully
trap 'echo ""; log_info "Test run interrupted"; exit 130' SIGINT

# Run main function
main "$@"