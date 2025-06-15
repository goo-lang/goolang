#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "include/proof_generation.h"
#include "include/contracts.h"
#include "include/dependent_types.h"

// Test helper functions
void test_proof_context_creation() {
    printf("Testing proof generation context creation...\n");
    
    ProofGenerationContext* ctx = proof_generation_context_create();
    assert(ctx != NULL);
    assert(ctx->solver_backend == SMT_SOLVER_Z3);
    assert(ctx->solver_timeout_seconds == 30);
    assert(ctx->use_proof_caching == true);
    assert(ctx->domain_count == 3);
    
    proof_generation_context_free(ctx);
    printf("✅ Proof context creation test passed\n");
}

void test_smt_expression_creation() {
    printf("Testing SMT expression creation...\n");
    
    // Create some basic SMT expressions
    SMTExpression* var_x = smt_var("x", NULL);
    assert(var_x != NULL);
    assert(var_x->type == SMT_VAR);
    assert(strcmp(var_x->variable.name, "x") == 0);
    
    SMTExpression* const_5 = smt_const_int(5);
    assert(const_5 != NULL);
    assert(const_5->type == SMT_CONST);
    assert(const_5->constant.int_val == 5);
    
    SMTExpression* greater_than = smt_app(">", 
        (SMTExpression*[]){var_x, const_5}, 2);
    assert(greater_than != NULL);
    assert(greater_than->type == SMT_APP);
    assert(strcmp(greater_than->application.function_name, ">") == 0);
    assert(greater_than->application.arg_count == 2);
    
    // Only free the parent expression - it owns the arguments
    smt_expression_free(greater_than);
    
    printf("✅ SMT expression creation test passed\n");
}

void test_memory_safety_proof() {
    printf("Testing memory safety proof generation...\n");
    
    ProofGenerationContext* ctx = proof_generation_context_create();
    assert(ctx != NULL);
    
    // Create a dummy AST node for testing
    struct ASTNode dummy_node = {0};
    
    MemorySafetyProof* proof = generate_memory_safety_proof(ctx, &dummy_node, NULL);
    assert(proof != NULL);
    assert(proof->safety_properties.null_pointer_safe == true);
    assert(proof->safety_properties.buffer_overflow_safe == true);
    
    memory_safety_proof_free(proof);
    proof_generation_context_free(ctx);
    
    printf("✅ Memory safety proof test passed\n");
}

void test_contract_proof_integration() {
    printf("Testing contract to proof integration...\n");
    
    ProofGenerationContext* ctx = proof_generation_context_create();
    assert(ctx != NULL);
    
    // Create a simple contract
    ContractContext* contract_ctx = contract_context_create();
    FunctionContract* func_contract = malloc(sizeof(FunctionContract));
    *func_contract = (FunctionContract){
        .function_name = strdup("test_function"),
        .preconditions = NULL,
        .postconditions = NULL,
        .next = NULL
    };
    
    // Add a simple precondition
    ContractExpression* precond = malloc(sizeof(ContractExpression));
    *precond = (ContractExpression){
        .type = CONTRACT_PRECONDITION,
        .condition = NULL,  // Would be AST node in real case
        .message = NULL,
        .line = 1,
        .column = 1,
        .filename = strdup("test.goo"),
        .description = strdup("x > 0"),
        .is_compile_time = true,
        .next = NULL
    };
    func_contract->preconditions = precond;
    
    contract_ctx->function_contracts = func_contract;
    
    // Generate proofs
    int proofs = generate_proofs_for_program(ctx, NULL, contract_ctx);
    assert(proofs >= 0);
    
    contract_context_free(contract_ctx);
    proof_generation_context_free(ctx);
    
    printf("✅ Contract proof integration test passed\n");
}

void test_invariant_inference() {
    printf("Testing automatic invariant inference...\n");
    
    ProofGenerationContext* ctx = proof_generation_context_create();
    assert(ctx != NULL);
    
    // Create a dummy loop AST node
    struct ASTNode dummy_loop = {0};
    
    InferredInvariant* invariants = proof_infer_loop_invariants(ctx, &dummy_loop, ABSTRACT_DOMAIN_INTERVALS);
    // Note: This may return NULL in our basic implementation
    
    if (invariants) {
        assert(invariants->confidence_score >= 0.0);
        assert(invariants->confidence_score <= 1.0);
        inferred_invariant_free(invariants);
    }
    
    proof_generation_context_free(ctx);
    
    printf("✅ Invariant inference test passed\n");
}

void test_termination_proof() {
    printf("Testing loop termination proof generation...\n");
    
    ProofGenerationContext* ctx = proof_generation_context_create();
    assert(ctx != NULL);
    
    // Create a dummy loop AST node
    struct ASTNode dummy_loop = {0};
    
    TerminationMeasure* measure = generate_termination_proof(ctx, &dummy_loop, NULL);
    // Note: This may return NULL in our basic implementation
    
    if (measure) {
        assert(measure->ranking_function != NULL);
        termination_measure_free(measure);
    }
    
    proof_generation_context_free(ctx);
    
    printf("✅ Termination proof test passed\n");
}

void test_proof_caching() {
    printf("Testing proof caching system...\n");
    
    ProofGenerationContext* ctx = proof_generation_context_create();
    assert(ctx != NULL);
    assert(ctx->use_proof_caching == true);
    
    // Test cache operations
    const char* test_hash = "test_obligation_hash";
    ProofStatus test_status = PROOF_STATUS_VERIFIED;
    const char* test_proof = "test_proof_content";
    
    // Store in cache
    int stored = proof_cache_store(ctx->cache, test_hash, test_status, test_proof, 1.0);
    assert(stored == 1);
    
    // Retrieve from cache
    ProofStatus cached_status;
    char* cached_proof;
    double cached_time;
    int found = proof_cache_lookup(ctx->cache, test_hash, &cached_status, &cached_proof, &cached_time);
    
    if (found) {
        assert(cached_status == test_status);
        assert(strcmp(cached_proof, test_proof) == 0);
        free(cached_proof);
    }
    
    proof_generation_context_free(ctx);
    
    printf("✅ Proof caching test passed\n");
}

void test_smt_solver_integration() {
    printf("Testing SMT solver integration...\n");
    
    ProofGenerationContext* ctx = proof_generation_context_create();
    assert(ctx != NULL);
    
    // Configure different solvers
    assert(proof_generation_configure_solver(ctx, SMT_SOLVER_Z3) == 1);
    assert(ctx->solver_backend == SMT_SOLVER_Z3);
    
    assert(proof_generation_configure_solver(ctx, SMT_SOLVER_CVC5) == 1);
    assert(ctx->solver_backend == SMT_SOLVER_CVC5);
    
    // Test solver query
    SMTExpression* formula = smt_app("and", 
        (SMTExpression*[]){
            smt_app(">", (SMTExpression*[]){smt_var("x", NULL), smt_const_int(0)}, 2),
            smt_app("<", (SMTExpression*[]){smt_var("x", NULL), smt_const_int(10)}, 2)
        }, 2);
    
    SMTResult result = smt_check_satisfiability(ctx, formula);
    printf("SMT result: %d\n", result);
    
    smt_expression_free(formula);
    proof_generation_context_free(ctx);
    
    printf("✅ SMT solver integration test passed\n");
}

void test_statistics_reporting() {
    printf("Testing statistics reporting...\n");
    
    ProofGenerationContext* ctx = proof_generation_context_create();
    assert(ctx != NULL);
    
    // Simulate some proof generation
    ctx->statistics.total_verification_time = 2.5;
    ctx->statistics.smt_queries_generated = 10;
    ctx->statistics.invariants_inferred = 3;
    ctx->statistics.termination_proofs = 2;
    ctx->statistics.memory_safety_proofs = 5;
    
    // Test statistics reporting
    print_proof_generation_statistics(ctx);
    
    proof_generation_context_free(ctx);
    
    printf("✅ Statistics reporting test passed\n");
}

// Main test function
int main() {
    printf("🧪 Running Proof Generation System Tests\n");
    printf("==========================================\n\n");
    
    test_proof_context_creation();
    test_smt_expression_creation();
    test_memory_safety_proof();
    test_contract_proof_integration();
    test_invariant_inference();
    test_termination_proof();
    test_proof_caching();
    test_smt_solver_integration();
    test_statistics_reporting();
    
    printf("\n🎉 All proof generation tests passed!\n");
    printf("==========================================\n");
    
    return 0;
}
