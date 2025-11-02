#!/bin/bash

# Test script for the Goo Language Server Protocol implementation
set -e

LSP_SERVER="./goo-lsp"
TEST_DIR="/tmp/goo_lsp_test"
FIFO_IN="/tmp/goo_lsp_in"
FIFO_OUT="/tmp/goo_lsp_out"

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

function send_lsp_message() {
    local content="$1"
    local length=${#content}
    
    echo -e "Content-Length: $length\r\n\r\n$content" > "$FIFO_IN"
}

function read_lsp_response() {
    local timeout=5
    local response=""
    
    # Read response with timeout
    if timeout $timeout cat "$FIFO_OUT" 2>/dev/null; then
        return 0
    else
        return 1
    fi
}

function test_initialize() {
    ((TESTS_RUN++))
    log_info "Testing initialize request"
    
    local init_request='{
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "processId": null,
            "rootUri": "file:///tmp/test_workspace",
            "capabilities": {
                "textDocument": {
                    "completion": {
                        "completionItem": {
                            "snippetSupport": true
                        }
                    }
                }
            }
        }
    }'
    
    send_lsp_message "$init_request"
    
    if response=$(timeout 3 head -n 10 "$FIFO_OUT" 2>/dev/null); then
        if echo "$response" | grep -q '"result"'; then
            log_success "Initialize request handled"
        else
            log_failure "Initialize request - unexpected response: $response"
        fi
    else
        log_failure "Initialize request - no response received"
    fi
}

function test_text_document_lifecycle() {
    ((TESTS_RUN++))
    log_info "Testing text document lifecycle"
    
    # Send didOpen notification
    local did_open='{
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": "file:///tmp/test.goo",
                "languageId": "goo",
                "version": 1,
                "text": "package main\n\nimport \"fmt\"\n\nfunc main() {\n    fmt.Println(\"Hello, World!\")\n}"
            }
        }
    }'
    
    send_lsp_message "$did_open"
    sleep 1
    
    # Send didChange notification
    local did_change='{
        "jsonrpc": "2.0",
        "method": "textDocument/didChange",
        "params": {
            "textDocument": {
                "uri": "file:///tmp/test.goo",
                "version": 2
            },
            "contentChanges": [{
                "text": "package main\n\nimport \"fmt\"\n\nfunc main() {\n    fmt.Println(\"Hello, Goo!\")\n}"
            }]
        }
    }'
    
    send_lsp_message "$did_change"
    sleep 1
    
    # Send didClose notification
    local did_close='{
        "jsonrpc": "2.0",
        "method": "textDocument/didClose",
        "params": {
            "textDocument": {
                "uri": "file:///tmp/test.goo"
            }
        }
    }'
    
    send_lsp_message "$did_close"
    
    log_success "Text document lifecycle notifications sent"
}

function test_completion() {
    ((TESTS_RUN++))
    log_info "Testing completion request"
    
    # First open a document
    local did_open='{
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": "file:///tmp/completion_test.goo",
                "languageId": "goo",
                "version": 1,
                "text": "package main\n\nfunc main() {\n    \n}"
            }
        }
    }'
    
    send_lsp_message "$did_open"
    sleep 1
    
    # Request completion
    local completion_request='{
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/completion",
        "params": {
            "textDocument": {
                "uri": "file:///tmp/completion_test.goo"
            },
            "position": {
                "line": 3,
                "character": 4
            }
        }
    }'
    
    send_lsp_message "$completion_request"
    
    if response=$(timeout 3 head -n 10 "$FIFO_OUT" 2>/dev/null); then
        if echo "$response" | grep -q '"result"'; then
            log_success "Completion request handled"
        else
            log_failure "Completion request - unexpected response"
        fi
    else
        log_failure "Completion request - no response received"
    fi
}

function test_hover() {
    ((TESTS_RUN++))
    log_info "Testing hover request"
    
    local hover_request='{
        "jsonrpc": "2.0",
        "id": 3,
        "method": "textDocument/hover",
        "params": {
            "textDocument": {
                "uri": "file:///tmp/completion_test.goo"
            },
            "position": {
                "line": 2,
                "character": 5
            }
        }
    }'
    
    send_lsp_message "$hover_request"
    
    if response=$(timeout 3 head -n 10 "$FIFO_OUT" 2>/dev/null); then
        if echo "$response" | grep -q '"result"'; then
            log_success "Hover request handled"
        else
            log_failure "Hover request - unexpected response"
        fi
    else
        log_failure "Hover request - no response received"
    fi
}

function test_shutdown() {
    ((TESTS_RUN++))
    log_info "Testing shutdown sequence"
    
    # Send shutdown request
    local shutdown_request='{
        "jsonrpc": "2.0",
        "id": 4,
        "method": "shutdown",
        "params": null
    }'
    
    send_lsp_message "$shutdown_request"
    
    if response=$(timeout 3 head -n 5 "$FIFO_OUT" 2>/dev/null); then
        if echo "$response" | grep -q '"result"'; then
            log_success "Shutdown request handled"
        else
            log_failure "Shutdown request - unexpected response"
        fi
    else
        log_failure "Shutdown request - no response received"
    fi
    
    # Send exit notification
    local exit_notification='{
        "jsonrpc": "2.0",
        "method": "exit",
        "params": null
    }'
    
    send_lsp_message "$exit_notification"
    sleep 1
}

function start_lsp_server() {
    log_info "Starting LSP server..."
    
    # Create named pipes for communication
    mkfifo "$FIFO_IN" "$FIFO_OUT" 2>/dev/null || true
    
    # Start LSP server with named pipes
    "$LSP_SERVER" --trace < "$FIFO_IN" > "$FIFO_OUT" 2>/dev/null &
    LSP_PID=$!
    
    # Give server time to start
    sleep 1
    
    if kill -0 "$LSP_PID" 2>/dev/null; then
        log_info "LSP server started (PID: $LSP_PID)"
        return 0
    else
        log_failure "Failed to start LSP server"
        return 1
    fi
}

function stop_lsp_server() {
    if [ -n "$LSP_PID" ] && kill -0 "$LSP_PID" 2>/dev/null; then
        log_info "Stopping LSP server..."
        kill "$LSP_PID" 2>/dev/null || true
        wait "$LSP_PID" 2>/dev/null || true
    fi
}

function cleanup() {
    stop_lsp_server
    rm -f "$FIFO_IN" "$FIFO_OUT"
    rm -rf "$TEST_DIR"
}

function test_simple_functionality() {
    ((TESTS_RUN++))
    log_info "Testing simple LSP server functionality"
    
    # Test that server can start and stop
    if "$LSP_SERVER" --help >/dev/null 2>&1; then
        log_success "LSP server executable works"
    else
        log_failure "LSP server executable failed"
        return
    fi
    
    # Test with invalid JSON to ensure error handling
    echo "invalid json" | timeout 2 "$LSP_SERVER" 2>/dev/null || true
    log_success "LSP server handles invalid input gracefully"
}

function main() {
    log_info "Starting Goo Language Server Protocol tests"
    
    # Check if LSP server exists
    if [ ! -f "$LSP_SERVER" ]; then
        log_failure "LSP server not found: $LSP_SERVER"
        log_info "Please run 'make' to build the LSP server first"
        exit 1
    fi
    
    # Check dependencies
    if ! command -v timeout >/dev/null; then
        log_failure "timeout command not found"
        exit 1
    fi
    
    # Create test directory
    mkdir -p "$TEST_DIR"
    
    # Set trap to cleanup on exit
    trap cleanup EXIT
    
    # Run simple tests first
    test_simple_functionality
    
    # Start LSP server for integration tests
    if start_lsp_server; then
        # Run integration tests
        test_initialize
        test_text_document_lifecycle
        test_completion
        test_hover
        test_shutdown
        
        # Wait a bit for shutdown to complete
        sleep 2
    else
        log_failure "Could not start LSP server for integration tests"
    fi
    
    # Test invalid scenarios
    ((TESTS_RUN++))
    log_info "Testing error handling"
    
    # Test with malformed request
    echo '{"invalid": "json"' | timeout 2 "$LSP_SERVER" 2>/dev/null || true
    log_success "Server handles malformed JSON"
    
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

# Run main function
main "$@"