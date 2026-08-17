#include "memory_safety.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Test framework for reference manager
static void test_print(const char* test_name, int passed) {
    printf("[%s] %s\n", passed ? "PASS" : "FAIL", test_name);
}

static int test_count = 0;
static int test_passed = 0;

#define RUN_TEST(test_func) do { \
    test_count++; \
    if (test_func()) { \
        test_passed++; \
        test_print(#test_func, 1); \
    } else { \
        test_print(#test_func, 0); \
    } \
} while(0)

#define ASSERT(condition, message) do { \
    if (!(condition)) { \
        printf("  ASSERTION FAILED: %s\n", message); \
        return 0; \
    } \
} while(0)

// Test functions
static int test_reference_manager_creation(void);
static int test_lifetime_scope_management(void);
static int test_reference_creation_and_tracking(void);
static int test_borrow_conflict_detection(void);
static int test_reference_invalidation(void);
static int test_move_safety_checks(void);
static int test_expression_analysis(void);
static int test_statement_analysis(void);
static int test_nested_scopes(void);
static int test_reference_manager_statistics(void);

// Test runner
int reference_manager_run_tests(void) {
    printf("Running Reference Manager Tests...\n");
    
    // Run tests
    RUN_TEST(test_reference_manager_creation);
    RUN_TEST(test_lifetime_scope_management);
    RUN_TEST(test_reference_creation_and_tracking);
    RUN_TEST(test_borrow_conflict_detection);
    RUN_TEST(test_reference_invalidation);
    RUN_TEST(test_move_safety_checks);
    RUN_TEST(test_expression_analysis);
    RUN_TEST(test_statement_analysis);
    RUN_TEST(test_nested_scopes);
    RUN_TEST(test_reference_manager_statistics);
    
    printf("Reference Manager Tests: %d/%d passed\n", test_passed, test_count);
    return test_passed == test_count;
}

// Test implementations

static int test_reference_manager_creation(void) {
    // Create a mock flow analyzer
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    
    // Create reference manager
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    ASSERT(mgr != NULL, "Reference manager should be created");
    ASSERT(mgr->flow_analyzer == flow_analyzer, "Flow analyzer should be set");
    ASSERT(mgr->global_scope != NULL, "Global scope should be created");
    ASSERT(mgr->current_scope == mgr->global_scope, "Current scope should be global");
    ASSERT(mgr->next_scope_id == 1, "Next scope ID should start at 1");
    ASSERT(mgr->enable_weak_references == 1, "Weak references should be enabled by default");
    ASSERT(mgr->enable_smart_pointers == 1, "Smart pointers should be enabled by default");
    ASSERT(mgr->strict_lifetime_checking == 1, "Strict checking should be enabled by default");
    
    // Cleanup
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

static int test_lifetime_scope_management(void) {
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    
    // Test entering a block scope
    LifetimeScope* block_scope = reference_manager_enter_scope(mgr, LIFETIME_SCOPE_BLOCK, 10);
    ASSERT(block_scope != NULL, "Block scope should be created");
    ASSERT(mgr->current_scope == block_scope, "Current scope should be the block scope");
    ASSERT(block_scope->kind == LIFETIME_SCOPE_BLOCK, "Scope kind should be block");
    ASSERT(block_scope->start_position == 10, "Start position should be 10");
    ASSERT(block_scope->parent == mgr->global_scope, "Parent should be global scope");
    ASSERT(block_scope->scope_id == 1, "Scope ID should be 1");
    
    // Test entering a nested scope
    LifetimeScope* nested_scope = reference_manager_enter_scope(mgr, LIFETIME_SCOPE_CONDITIONAL, 20);
    ASSERT(nested_scope != NULL, "Nested scope should be created");
    ASSERT(mgr->current_scope == nested_scope, "Current scope should be the nested scope");
    ASSERT(nested_scope->parent == block_scope, "Parent should be block scope");
    ASSERT(nested_scope->scope_id == 2, "Scope ID should be 2");
    
    // Test exiting scopes
    reference_manager_exit_scope(mgr, 30);
    ASSERT(mgr->current_scope == block_scope, "Should return to block scope");
    
    reference_manager_exit_scope(mgr, 40);
    ASSERT(mgr->current_scope == mgr->global_scope, "Should return to global scope");
    
    // Cleanup
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

static int test_reference_creation_and_tracking(void) {
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    
    // Create a reference
    ReferenceInfo* ref = reference_manager_create_reference(mgr, "ref1", "var1", REFERENCE_KIND_SHARED, 10);
    ASSERT(ref != NULL, "Reference should be created");
    ASSERT(strcmp(ref->name, "ref1") == 0, "Reference name should be ref1");
    ASSERT(strcmp(ref->target_name, "var1") == 0, "Target name should be var1");
    ASSERT(ref->kind == REFERENCE_KIND_SHARED, "Reference kind should be shared");
    ASSERT(ref->creation_position == 10, "Creation position should be 10");
    ASSERT(ref->validity == REFERENCE_VALID, "Reference should be valid");
    ASSERT(mgr->reference_count == 1, "Reference count should be 1");
    ASSERT(mgr->references_created == 1, "Statistics should be updated");
    
    // Create another reference to the same target
    ReferenceInfo* ref2 = reference_manager_create_reference(mgr, "ref2", "var1", REFERENCE_KIND_SHARED, 20);
    ASSERT(ref2 != NULL, "Second reference should be created");
    ASSERT(mgr->reference_count == 2, "Reference count should be 2");
    
    // Test reference usage tracking
    reference_manager_use_reference(mgr, "ref1", 25);
    ASSERT(ref->last_use_position == 25, "Last use position should be updated");
    
    // Cleanup
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

static int test_borrow_conflict_detection(void) {
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    
    // Create an immutable reference
    ReferenceInfo* ref1 = reference_manager_create_reference(mgr, "ref1", "var1", REFERENCE_KIND_SHARED, 10);
    ASSERT(ref1 != NULL, "Immutable reference should be created");
    ASSERT(mgr->error_count == 0, "No errors should occur");
    
    // Create another immutable reference to the same target (should succeed)
    ReferenceInfo* ref2 = reference_manager_create_reference(mgr, "ref2", "var1", REFERENCE_KIND_SHARED, 20);
    ASSERT(ref2 != NULL, "Second immutable reference should be created");
    ASSERT(mgr->error_count == 0, "No errors should occur");
    
    // Try to create a mutable reference (should fail due to existing immutable references)
    ReferenceInfo* ref3 = reference_manager_create_reference(mgr, "ref3", "var1", REFERENCE_KIND_MUTABLE, 30);
    ASSERT(ref3 == NULL, "Mutable reference should fail to create");
    ASSERT(mgr->error_count == 1, "Error should be reported");
    ASSERT(mgr->borrow_conflicts_detected == 1, "Borrow conflict should be detected");
    
    // Cleanup
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

static int test_reference_invalidation(void) {
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    
    // Create a reference
    ReferenceInfo* ref = reference_manager_create_reference(mgr, "ref1", "var1", REFERENCE_KIND_SHARED, 10);
    ASSERT(ref != NULL, "Reference should be created");
    ASSERT(ref->validity == REFERENCE_VALID, "Reference should be valid");
    
    // Invalidate references to the target
    reference_manager_invalidate_references(mgr, "var1", 20);
    ASSERT(ref->validity == REFERENCE_INVALIDATED, "Reference should be invalidated");
    ASSERT(ref->last_use_position == 20, "Last use position should be updated");
    ASSERT(mgr->references_invalidated == 1, "Statistics should be updated");
    
    // Try to use the invalidated reference
    int initial_error_count = mgr->error_count;
    reference_manager_use_reference(mgr, "ref1", 25);
    ASSERT(mgr->error_count > initial_error_count, "Error should be reported for using invalidated reference");
    
    // Cleanup
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

static int test_move_safety_checks(void) {
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    
    // Test moving a variable with no references (should succeed)
    int can_move = reference_manager_can_move(mgr, "var1", 10);
    ASSERT(can_move == 1, "Variable with no references should be movable");
    
    // Create a reference and test moving (should fail)
    reference_manager_create_reference(mgr, "ref1", "var1", REFERENCE_KIND_SHARED, 10);
    can_move = reference_manager_can_move(mgr, "var1", 20);
    ASSERT(can_move == 0, "Variable with active references should not be movable");
    
    // Invalidate the reference and test moving again (should succeed)
    reference_manager_invalidate_references(mgr, "var1", 30);
    can_move = reference_manager_can_move(mgr, "var1", 40);
    ASSERT(can_move == 1, "Variable with no active references should be movable");
    
    // Cleanup
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

static int test_expression_analysis(void) {
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    
    // Create a simple identifier node
    IdentifierNode* ident = calloc(1, sizeof(IdentifierNode));
    ident->base.type = AST_IDENTIFIER;
    ident->name = strdup("test_var");
    
    // Analyze the identifier expression
    int result = reference_manager_analyze_expression(mgr, (ASTNode*)ident, 10);
    ASSERT(result == 1, "Expression analysis should succeed");
    
    // Cleanup
    free(ident->name);
    free(ident);
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

static int test_statement_analysis(void) {
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    
    // Create a simple expression statement
    ExprStmtNode* expr_stmt = calloc(1, sizeof(ExprStmtNode));
    expr_stmt->base.type = AST_EXPR_STMT;
    
    IdentifierNode* ident = calloc(1, sizeof(IdentifierNode));
    ident->base.type = AST_IDENTIFIER;
    ident->name = strdup("test_var");
    expr_stmt->expr = (ASTNode*)ident;
    
    // Analyze the statement
    int result = reference_manager_analyze_statement(mgr, (ASTNode*)expr_stmt, 10);
    ASSERT(result == 1, "Statement analysis should succeed");
    
    // Cleanup
    free(ident->name);
    free(ident);
    free(expr_stmt);
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

static int test_nested_scopes(void) {
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    
    // Enter a function scope
    LifetimeScope* func_scope = reference_manager_enter_scope(mgr, LIFETIME_SCOPE_FUNCTION, 0);
    
    // Create a reference in the function scope
    reference_manager_create_reference(mgr, "ref1", "var1", REFERENCE_KIND_SHARED, 10);
    ASSERT(func_scope->reference_count == 1, "Function scope should have 1 reference");
    
    // Enter a nested block scope
    LifetimeScope* block_scope = reference_manager_enter_scope(mgr, LIFETIME_SCOPE_BLOCK, 20);
    
    // Create a reference in the block scope
    reference_manager_create_reference(mgr, "ref2", "var2", REFERENCE_KIND_SHARED, 30);
    ASSERT(block_scope->reference_count == 1, "Block scope should have 1 reference");
    ASSERT(func_scope->reference_count == 1, "Function scope should still have 1 reference");
    
    // Exit the block scope - references should be invalidated
    reference_manager_exit_scope(mgr, 40);
    ASSERT(mgr->current_scope == func_scope, "Should return to function scope");
    ASSERT(mgr->references_invalidated == 1, "Block scope reference should be invalidated");
    
    // Exit the function scope
    reference_manager_exit_scope(mgr, 50);
    ASSERT(mgr->current_scope == mgr->global_scope, "Should return to global scope");
    ASSERT(mgr->references_invalidated == 2, "Function scope reference should be invalidated");
    
    // Cleanup
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

static int test_reference_manager_statistics(void) {
    FlowSensitiveAnalyzer* flow_analyzer = calloc(1, sizeof(FlowSensitiveAnalyzer));
    ReferenceManager* mgr = reference_manager_new(flow_analyzer);
    
    // Initial statistics
    ASSERT(mgr->references_created == 0, "Initial references created should be 0");
    ASSERT(mgr->references_invalidated == 0, "Initial references invalidated should be 0");
    ASSERT(mgr->borrow_conflicts_detected == 0, "Initial conflicts should be 0");
    ASSERT(mgr->error_count == 0, "Initial errors should be 0");
    
    // Create some references
    reference_manager_create_reference(mgr, "ref1", "var1", REFERENCE_KIND_SHARED, 10);
    reference_manager_create_reference(mgr, "ref2", "var2", REFERENCE_KIND_SHARED, 20);
    ASSERT(mgr->references_created == 2, "References created should be 2");
    
    // Cause a borrow conflict
    reference_manager_create_reference(mgr, "ref3", "var1", REFERENCE_KIND_MUTABLE, 30);
    ASSERT(mgr->borrow_conflicts_detected == 1, "Borrow conflicts should be 1");
    ASSERT(mgr->error_count == 1, "Error count should be 1");
    
    // Invalidate some references
    reference_manager_invalidate_references(mgr, "var1", 40);
    reference_manager_invalidate_references(mgr, "var2", 50);
    ASSERT(mgr->references_invalidated == 2, "References invalidated should be 2");
    
    // Print statistics (for manual inspection)
    printf("  Printing reference manager statistics:\n");
    reference_manager_print_statistics(mgr);
    
    // Cleanup
    reference_manager_free(mgr);
    free(flow_analyzer);
    
    return 1;
}

#ifdef STANDALONE_TEST
int main(void) {
    return reference_manager_run_tests() ? 0 : 1;
}
#endif
