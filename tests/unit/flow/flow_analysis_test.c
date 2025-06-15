#include "memory_safety.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Test framework for flow-sensitive ownership analysis

// Test utilities
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

// Mock type checker for testing
static TypeChecker* create_mock_type_checker() {
    TypeChecker* checker = type_checker_new();
    if (checker) {
        type_checker_init_builtins(checker);
    }
    return checker;
}

// Test value state management
static int test_value_state_creation() {
    ValueState* state = value_state_new("test_var", VALUE_STATE_INITIALIZED);
    if (!state) return 0;
    
    int result = (strcmp(state->name, "test_var") == 0) &&
                 (state->state == VALUE_STATE_INITIALIZED) &&
                 (state->recommended_transfer == TRANSFER_KIND_AUTO_INFERRED);
    
    value_state_free(state);
    return result;
}

static int test_value_state_copy() {
    ValueState* original = value_state_new("original", VALUE_STATE_MOVED);
    original->escape_status = ESCAPE_FUNCTION;
    original->ref_count = 5;
    
    ValueState* copy = value_state_copy(original);
    if (!copy) {
        value_state_free(original);
        return 0;
    }
    
    int result = (strcmp(copy->name, "original") == 0) &&
                 (copy->state == VALUE_STATE_MOVED) &&
                 (copy->escape_status == ESCAPE_FUNCTION) &&
                 (copy->ref_count == 5);
    
    value_state_free(original);
    value_state_free(copy);
    return result;
}

static int test_value_state_merge() {
    ValueState* state1 = value_state_new("test", VALUE_STATE_INITIALIZED);
    ValueState* state2 = value_state_new("test", VALUE_STATE_MOVED);
    
    state1->ref_count = 2;
    state2->ref_count = 5;
    state2->escape_status = ESCAPE_CLOSURE;
    
    value_state_merge(state1, state2);
    
    int result = (state1->state == VALUE_STATE_CONDITIONALLY_MOVED) &&
                 (state1->ref_count == 5) &&
                 (state1->escape_status == ESCAPE_CLOSURE);
    
    value_state_free(state1);
    value_state_free(state2);
    return result;
}

// Test ownership state management
static int test_ownership_state_creation() {
    OwnershipState* state = ownership_state_new(NULL);
    if (!state) return 0;
    
    int result = (state->value_count == 0) &&
                 (state->values != NULL) &&
                 (state->parent == NULL);
    
    ownership_state_free(state);
    return result;
}

static int test_ownership_state_add_value() {
    OwnershipState* state = ownership_state_new(NULL);
    ValueState* value = value_state_new("test_var", VALUE_STATE_INITIALIZED);
    
    ownership_state_add_value(state, value);
    
    int result = (state->value_count == 1) &&
                 (state->values[0] == value);
    
    ownership_state_free(state);  // This will free the value too
    return result;
}

static int test_ownership_state_lookup() {
    OwnershipState* state = ownership_state_new(NULL);
    ValueState* value = value_state_new("lookup_test", VALUE_STATE_BORROWED_IMMUTABLE);
    
    ownership_state_add_value(state, value);
    
    ValueState* found = ownership_state_lookup(state, "lookup_test");
    ValueState* not_found = ownership_state_lookup(state, "nonexistent");
    
    int result = (found == value) && (not_found == NULL);
    
    ownership_state_free(state);
    return result;
}

static int test_ownership_state_update() {
    OwnershipState* state = ownership_state_new(NULL);
    
    // Update non-existent variable (should create it)
    ownership_state_update(state, "new_var", VALUE_STATE_MOVED);
    
    ValueState* value = ownership_state_lookup(state, "new_var");
    int result1 = (value != NULL) && (value->state == VALUE_STATE_MOVED);
    
    // Update existing variable
    ownership_state_update(state, "new_var", VALUE_STATE_INITIALIZED);
    int result2 = (value->state == VALUE_STATE_INITIALIZED);
    
    ownership_state_free(state);
    return result1 && result2;
}

// Test control flow graph
static int test_cfg_creation() {
    ControlFlowGraph* cfg = malloc(sizeof(ControlFlowGraph));
    if (!cfg) return 0;
    
    cfg->blocks = NULL;
    cfg->block_count = 0;
    cfg->block_capacity = 16;
    cfg->blocks = malloc(sizeof(BasicBlock*) * cfg->block_capacity);
    
    BasicBlock* entry = cfg_add_block(cfg);
    BasicBlock* exit = cfg_add_block(cfg);
    
    cfg->entry_block = entry;
    cfg->exit_block = exit;
    cfg->dominators = NULL;
    cfg->dominated_sets = NULL;
    cfg->loops = NULL;
    cfg->loop_count = 0;
    
    int result = (cfg->block_count == 2) &&
                 (entry->id == 0) &&
                 (exit->id == 1);
    
    cfg_free(cfg);
    return result;
}

static int test_cfg_add_edge() {
    ControlFlowGraph* cfg = malloc(sizeof(ControlFlowGraph));
    cfg->blocks = malloc(sizeof(BasicBlock*) * 16);
    cfg->block_count = 0;
    cfg->block_capacity = 16;
    cfg->dominators = NULL;
    cfg->dominated_sets = NULL;
    cfg->loops = NULL;
    cfg->loop_count = 0;
    
    BasicBlock* block1 = cfg_add_block(cfg);
    BasicBlock* block2 = cfg_add_block(cfg);
    
    cfg_add_edge(block1, block2);
    
    int result = (block1->successor_count == 1) &&
                 (block1->successors[0] == block2) &&
                 (block2->predecessor_count == 1) &&
                 (block2->predecessors[0] == block1);
    
    cfg_free(cfg);
    return result;
}

// Test flow analyzer
static int test_flow_analyzer_creation() {
    TypeChecker* checker = create_mock_type_checker();
    FlowSensitiveAnalyzer* analyzer = flow_analyzer_new(checker);
    
    int result = (analyzer != NULL) &&
                 (analyzer->type_checker == checker) &&
                 (analyzer->cfg == NULL) &&
                 (analyzer->error_count == 0);
    
    flow_analyzer_free(analyzer);
    type_checker_free(checker);
    return result;
}

// Test transfer operation inference
static int test_transfer_operation_inference() {
    TypeChecker* checker = create_mock_type_checker();
    FlowSensitiveAnalyzer* analyzer = flow_analyzer_new(checker);
    OwnershipState* state = ownership_state_new(NULL);
    
    // Create a test value state
    ValueState* value = value_state_new("test_var", VALUE_STATE_INITIALIZED);
    value->escape_status = ESCAPE_FUNCTION;
    ownership_state_add_value(state, value);
    
    // Create a mock AST node for testing
    IdentifierNode* ident = malloc(sizeof(IdentifierNode));
    ident->base.type = AST_IDENTIFIER;
    ident->base.pos = (Position){0};
    ident->name = malloc(strlen("test_var") + 1);
    strcpy(ident->name, "test_var");
    
    TransferKind transfer = infer_transfer_operation(analyzer, "test_var", 
                                                   (ASTNode*)ident, state);
    
    // Since escape_status is ESCAPE_FUNCTION, should prefer move
    int result = (transfer == TRANSFER_KIND_MOVE);
    
    free(ident->name);
    free(ident);
    ownership_state_free(state);
    flow_analyzer_free(analyzer);
    type_checker_free(checker);
    return result;
}

// Test conditional state merging
static int test_conditional_state_merge() {
    OwnershipState* then_state = ownership_state_new(NULL);
    OwnershipState* else_state = ownership_state_new(NULL);
    OwnershipState* result_state = ownership_state_new(NULL);
    
    // Set up different states in branches
    ValueState* then_value = value_state_new("branch_var", VALUE_STATE_MOVED);
    ValueState* else_value = value_state_new("branch_var", VALUE_STATE_INITIALIZED);
    
    ownership_state_add_value(then_state, then_value);
    ownership_state_add_value(else_state, else_value);
    
    int merge_result = merge_conditional_states(then_state, else_state, result_state);
    
    ValueState* merged = ownership_state_lookup(result_state, "branch_var");
    
    int result = merge_result &&
                 (merged != NULL) &&
                 (merged->state == VALUE_STATE_CONDITIONALLY_MOVED);
    
    ownership_state_free(then_state);
    ownership_state_free(else_state);
    ownership_state_free(result_state);
    return result;
}

// Test string conversion utilities
static int test_state_string_conversion() {
    const char* uninitialized = value_state_to_string(VALUE_STATE_UNINITIALIZED);
    const char* moved = value_state_to_string(VALUE_STATE_MOVED);
    const char* copy = transfer_kind_to_string(TRANSFER_KIND_COPY);
    const char* escape_func = escape_kind_to_string(ESCAPE_FUNCTION);
    
    return (strcmp(uninitialized, "uninitialized") == 0) &&
           (strcmp(moved, "moved") == 0) &&
           (strcmp(copy, "copy") == 0) &&
           (strcmp(escape_func, "function") == 0);
}

// Integration test with simple function
static int test_simple_function_analysis() {
    TypeChecker* checker = create_mock_type_checker();
    FlowSensitiveAnalyzer* analyzer = flow_analyzer_new(checker);
    
    // Create a simple function AST for testing
    FuncDeclNode* func = malloc(sizeof(FuncDeclNode));
    func->base.type = AST_FUNC_DECL;
    func->base.pos = (Position){0};
    func->name = malloc(strlen("test_func") + 1);
    strcpy(func->name, "test_func");
    func->params = NULL;
    func->return_type = NULL;
    func->body = NULL;  // Empty function
    
    int result = flow_analyze_function(analyzer, (ASTNode*)func);
    
    free(func->name);
    free(func);
    flow_analyzer_free(analyzer);
    type_checker_free(checker);
    
    return result;
}

// Run all tests
void run_flow_analysis_tests() {
    printf("\n=== Flow-Sensitive Ownership Analysis Tests ===\n");
    
    // Value state tests
    RUN_TEST(test_value_state_creation);
    RUN_TEST(test_value_state_copy);
    RUN_TEST(test_value_state_merge);
    
    // Ownership state tests
    RUN_TEST(test_ownership_state_creation);
    RUN_TEST(test_ownership_state_add_value);
    RUN_TEST(test_ownership_state_lookup);
    RUN_TEST(test_ownership_state_update);
    
    // Control flow graph tests
    RUN_TEST(test_cfg_creation);
    RUN_TEST(test_cfg_add_edge);
    
    // Flow analyzer tests
    RUN_TEST(test_flow_analyzer_creation);
    RUN_TEST(test_transfer_operation_inference);
    RUN_TEST(test_conditional_state_merge);
    
    // Utility tests
    RUN_TEST(test_state_string_conversion);
    
    // Integration tests
    RUN_TEST(test_simple_function_analysis);
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d/%d tests\n", test_passed, test_count);
    printf("Success rate: %.1f%%\n", (test_passed * 100.0) / test_count);
    
    if (test_passed == test_count) {
        printf("All tests passed! ✓\n");
    } else {
        printf("%d tests failed ✗\n", test_count - test_passed);
    }
}

// Test entry point
int main() {
    run_flow_analysis_tests();
    return (test_passed == test_count) ? 0 : 1;
}
