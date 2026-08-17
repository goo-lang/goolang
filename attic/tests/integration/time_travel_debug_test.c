#include "time_travel_debug.h"
#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void test_debug_state_creation(void) {
    printf("Testing debug state creation...\n");
    
    DebugState* state = debug_state_new();
    assert(state != NULL);
    assert(state->timeline != NULL);
    assert(!debug_is_enabled(state));
    assert(!debug_is_in_session(state));
    
    debug_state_free(state);
    printf("✓ Debug state creation test passed\n");
}

void test_debug_enable_disable(void) {
    printf("Testing debug enable/disable...\n");
    
    DebugState* state = debug_state_new();
    assert(state != NULL);
    
    // Test enabling debug
    debug_enable(state);
    assert(debug_is_enabled(state));
    
    // Test disabling debug
    debug_disable(state);
    assert(!debug_is_enabled(state));
    
    debug_state_free(state);
    printf("✓ Debug enable/disable test passed\n");
}

void test_debug_session_management(void) {
    printf("Testing debug session management...\n");
    
    DebugState* state = debug_state_new();
    assert(state != NULL);
    
    // Test starting session
    bool result = debug_start_session(state, "Test Session");
    assert(result);
    assert(debug_is_in_session(state));
    
    // Test ending session
    debug_end_session(state);
    assert(!debug_is_in_session(state));
    
    debug_state_free(state);
    printf("✓ Debug session management test passed\n");
}

void test_snapshot_creation(void) {
    printf("Testing snapshot creation...\n");
    
    DebugState* state = debug_state_new();
    assert(state != NULL);
    
    debug_enable(state);
    debug_start_session(state, "Snapshot Test");
    
    // Create some snapshots
    debug_record_expression_start(state, "1 + 2", (SourceLocation){.filename = "test", .line = 1, .column = 1});
    debug_record_expression_end(state, "1 + 2", NULL);
    debug_create_manual_snapshot(state, "Manual test snapshot");
    
    // Check timeline has snapshots
    assert(state->timeline->snapshot_count > 0);
    assert(state->timeline->current != NULL);
    
    debug_state_free(state);
    printf("✓ Snapshot creation test passed\n");
}

void test_navigation(void) {
    printf("Testing time-travel navigation...\n");
    
    DebugState* state = debug_state_new();
    assert(state != NULL);
    
    debug_enable(state);
    debug_start_session(state, "Navigation Test");
    
    // Create multiple snapshots
    for (int i = 0; i < 5; i++) {
        char desc[64];
        snprintf(desc, sizeof(desc), "Step %d", i);
        debug_create_manual_snapshot(state, desc);
    }
    
    // Test navigation
    uint64_t end_step = debug_get_current_step(state);
    assert(debug_is_at_end(state));
    
    // Step backward
    bool success = debug_step_backward(state);
    assert(success);
    assert(debug_get_current_step(state) < end_step);
    
    // Step forward
    success = debug_step_forward(state);
    assert(success);
    assert(debug_get_current_step(state) == end_step);
    
    // Go to beginning
    success = debug_continue_backward(state);
    assert(success);
    assert(debug_is_at_beginning(state));
    
    // Go to end
    success = debug_continue_forward(state);
    assert(success);
    assert(debug_is_at_end(state));
    
    debug_state_free(state);
    printf("✓ Navigation test passed\n");
}

void test_repl_integration(void) {
    printf("Testing REPL integration...\n");
    
    REPLContext* ctx = repl_context_new();
    assert(ctx != NULL);
    
    // Initialize debug system
    bool success = repl_debug_init(ctx);
    assert(success);
    assert(ctx->debug_state != NULL);
    
    // Test debug enable command
    int result = repl_cmd_debug_enable(ctx, "");
    assert(result == 0);
    assert(debug_is_enabled(ctx->debug_state));
    
    // Test manual snapshot command
    result = repl_cmd_debug_snapshot(ctx, "Test REPL snapshot");
    assert(result == 0);
    
    // Test timeline command
    result = repl_cmd_debug_timeline(ctx, "");
    assert(result == 0);
    
    // Test debug disable command
    result = repl_cmd_debug_disable(ctx, "");
    assert(result == 0);
    assert(!debug_is_enabled(ctx->debug_state));
    
    repl_context_free(ctx);
    printf("✓ REPL integration test passed\n");
}

int main(void) {
    printf("Running Time-Travel Debug Tests...\n\n");
    
    test_debug_state_creation();
    test_debug_enable_disable();
    test_debug_session_management();
    test_snapshot_creation();
    test_navigation();
    test_repl_integration();
    
    printf("\n✅ All time-travel debug tests passed!\n");
    return 0;
}