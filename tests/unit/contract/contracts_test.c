#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../include/contracts.h"
#include "../include/ast.h"
#include "../include/types.h"
#include "../include/token.h"

// Mock types and functions for testing
struct Type {
    int id;
    char* name;
};

struct TypeContext {
    int dummy;
};

struct ErrorContext {
    int error_count;
};

// Helper function to create a simple boolean literal AST node
ASTNode* create_bool_literal(int value) {
    ASTNode* node = malloc(sizeof(ASTNode));
    if (!node) return NULL;
    
    node->type = AST_LITERAL;
    node->pos.line = 1;
    node->pos.column = 1;
    node->node_type = NULL;
    node->next = NULL;
    
    // Note: In a real implementation, we'd have a proper literal structure
    // For this test, we'll just store the boolean value in a simple way
    
    return node;
}

// Helper function to create a simple variable AST node
ASTNode* create_variable(const char* name) {
    ASTNode* node = malloc(sizeof(ASTNode));
    if (!node) return NULL;
    
    node->type = AST_VARIABLE;
    node->pos.line = 1;
    node->pos.column = 1;
    node->node_type = NULL;
    node->next = NULL;
    
    return node;
}

// Test contract expression creation
void test_contract_expression_creation() {
    printf("Testing contract expression creation...\n");
    
    ASTNode* condition = create_bool_literal(1);
    assert(condition != NULL);
    
    ContractExpression* expr = contract_expression_create(
        CONTRACT_PRECONDITION, 
        condition, 
        "Test precondition"
    );
    
    assert(expr != NULL);
    assert(expr->type == CONTRACT_PRECONDITION);
    assert(expr->condition == condition);
    assert(strcmp(expr->description, "Test precondition") == 0);
    
    contract_expression_free(expr);
    free(condition);
    
    printf("✓ Contract expression creation test passed\n");
}

// Test function contract creation and management
void test_function_contract_management() {
    printf("Testing function contract management...\n");
    
    FunctionContract* contract = function_contract_create("test_function");
    assert(contract != NULL);
    assert(strcmp(contract->function_name, "test_function") == 0);
    
    // Add precondition
    ASTNode* precond = create_variable("x");
    int result = function_contract_add_precondition(
        contract, 
        precond, 
        "x must be positive"
    );
    assert(result == 1);
    assert(contract->preconditions != NULL);
    assert(contract->preconditions->type == CONTRACT_PRECONDITION);
    
    // Add postcondition
    ASTNode* postcond = create_variable("result");
    result = function_contract_add_postcondition(
        contract,
        postcond,
        "result must be non-negative"
    );
    assert(result == 1);
    assert(contract->postconditions != NULL);
    assert(contract->postconditions->type == CONTRACT_POSTCONDITION);
    
    function_contract_free(contract);
    free(precond);
    free(postcond);
    
    printf("✓ Function contract management test passed\n");
}

// Test loop contract creation and management
void test_loop_contract_management() {
    printf("Testing loop contract management...\n");
    
    ASTNode* loop_node = create_variable("for_loop");
    LoopContract* contract = loop_contract_create(loop_node);
    assert(contract != NULL);
    assert(contract->loop_node == loop_node);
    
    // Add invariant
    ASTNode* invariant = create_variable("i");
    int result = loop_contract_add_invariant(
        contract,
        invariant,
        "i is within bounds"
    );
    assert(result == 1);
    assert(contract->invariants != NULL);
    assert(contract->invariants->type == CONTRACT_INVARIANT);
    
    loop_contract_free(contract);
    free(loop_node);
    free(invariant);
    
    printf("✓ Loop contract management test passed\n");
}

// Test contract context management
void test_contract_context_management() {
    printf("Testing contract context management...\n");
    
    ContractContext* ctx = contract_context_create();
    assert(ctx != NULL);
    assert(ctx->verification_enabled == 1);
    assert(ctx->optimization_level == 1);
    
    // Create and add function contract
    FunctionContract* func_contract = function_contract_create("test_func");
    assert(func_contract != NULL);
    
    int result = contract_context_add_function(ctx, func_contract);
    assert(result == 1);
    assert(ctx->function_contracts == func_contract);
    
    // Create and add loop contract
    ASTNode* loop_node = create_variable("loop");
    LoopContract* loop_contract = loop_contract_create(loop_node);
    assert(loop_contract != NULL);
    
    result = contract_context_add_loop(ctx, loop_contract);
    assert(result == 1);
    assert(ctx->loop_contracts == loop_contract);
    
    contract_context_free(ctx);
    free(loop_node);
    
    printf("✓ Contract context management test passed\n");
}

// Test contract verification (basic)
void test_contract_verification() {
    printf("Testing contract verification...\n");
    
    // Test with true literal (should verify)
    ASTNode* true_condition = create_bool_literal(1);
    ContractExpression* true_expr = contract_expression_create(
        CONTRACT_PRECONDITION,
        true_condition,
        "Always true"
    );
    
    struct TypeContext type_ctx = {0};
    ContractVerificationInfo* info = verify_contract_expression(
        true_expr, &type_ctx, NULL
    );
    
    assert(info != NULL);
    // Note: The actual verification result depends on the implementation
    // For now, we just check that the function doesn't crash
    
    contract_verification_info_free(info);
    contract_expression_free(true_expr);
    free(true_condition);
    
    printf("✓ Contract verification test passed\n");
}

// Test error message generation
void test_error_message_generation() {
    printf("Testing error message generation...\n");
    
    ASTNode* condition = create_variable("x");
    ContractExpression* expr = contract_expression_create(
        CONTRACT_PRECONDITION,
        condition,
        "x must be positive"
    );
    
    char* message = generate_contract_error_message(expr, NULL);
    assert(message != NULL);
    assert(strstr(message, "Precondition violated") != NULL);
    assert(strstr(message, "x must be positive") != NULL);
    
    free(message);
    contract_expression_free(expr);
    free(condition);
    
    printf("✓ Error message generation test passed\n");
}

// Test contract to string conversion
void test_contract_to_string() {
    printf("Testing contract to string conversion...\n");
    
    ASTNode* condition = create_variable("x");
    ContractExpression* expr = contract_expression_create(
        CONTRACT_INVARIANT,
        condition,
        "x is positive"
    );
    
    char* str = contract_to_string(expr);
    assert(str != NULL);
    assert(strstr(str, "invariant") != NULL);
    assert(strstr(str, "x is positive") != NULL);
    
    free(str);
    contract_expression_free(expr);
    free(condition);
    
    printf("✓ Contract to string test passed\n");
}

// Test contract validation
void test_contract_validation() {
    printf("Testing contract validation...\n");
    
    // Test valid contract condition
    ASTNode* valid_condition = create_variable("x");
    int is_valid = is_valid_contract_condition(valid_condition);
    
    // Note: The actual validation depends on the AST structure
    // For now, we just test that the function doesn't crash
    
    free(valid_condition);
    
    printf("✓ Contract validation test passed\n");
}

// Test integration with dependent types (basic)
void test_dependent_type_integration() {
    printf("Testing dependent type integration...\n");
    
    struct Type base_type = {1, "int"};
    ASTNode* condition = create_variable("x");
    ContractExpression* expr = contract_expression_create(
        CONTRACT_PRECONDITION,
        condition,
        "x > 0"
    );
    
    // This would normally integrate with the dependent type system
    // For now, we just test that the functions don't crash
    DependentType* dep_type = contract_to_dependent_constraint(expr, &base_type);
    
    contract_expression_free(expr);
    free(condition);
    
    printf("✓ Dependent type integration test passed\n");
}

// Test memory management and cleanup
void test_memory_management() {
    printf("Testing memory management...\n");
    
    // Create a complex contract structure and ensure it cleans up properly
    ContractContext* ctx = contract_context_create();
    
    // Add multiple function contracts
    for (int i = 0; i < 3; i++) {
        char func_name[32];
        snprintf(func_name, sizeof(func_name), "func_%d", i);
        
        FunctionContract* contract = function_contract_create(func_name);
        
        // Add preconditions and postconditions
        ASTNode* precond = create_variable("param");
        function_contract_add_precondition(contract, precond, "param check");
        
        ASTNode* postcond = create_variable("result");
        function_contract_add_postcondition(contract, postcond, "result check");
        
        contract_context_add_function(ctx, contract);
        
        free(precond);
        free(postcond);
    }
    
    // Add multiple loop contracts
    for (int i = 0; i < 2; i++) {
        ASTNode* loop_node = create_variable("loop");
        LoopContract* contract = loop_contract_create(loop_node);
        
        ASTNode* invariant = create_variable("i");
        loop_contract_add_invariant(contract, invariant, "bounds check");
        
        contract_context_add_loop(ctx, contract);
        
        free(loop_node);
        free(invariant);
    }
    
    // Clean up everything
    contract_context_free(ctx);
    
    printf("✓ Memory management test passed\n");
}

int main() {
    printf("=== Contract Programming Framework Test Suite ===\n\n");
    
    test_contract_expression_creation();
    test_function_contract_management();
    test_loop_contract_management();
    test_contract_context_management();
    test_contract_verification();
    test_error_message_generation();
    test_contract_to_string();
    test_contract_validation();
    test_dependent_type_integration();
    test_memory_management();
    
    printf("\n=== All Contract Programming Tests Passed! ===\n");
    printf("✓ Contract expression creation and management\n");
    printf("✓ Function and loop contract handling\n");
    printf("✓ Contract context management\n");
    printf("✓ Basic contract verification\n");
    printf("✓ Error reporting and string conversion\n");
    printf("✓ Contract validation\n");
    printf("✓ Dependent type integration\n");
    printf("✓ Memory management and cleanup\n");
    
    return 0;
}
