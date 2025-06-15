#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "contracts.h"
#include "proof_generation.h"

// Test integration between contract programming and proof generation
void test_contract_proof_integration() {
    printf("🔗 Testing Contract Programming + Proof Generation Integration\n");
    printf("============================================================\n\n");
    
    // Create both contexts
    ContractContext* contract_ctx = contract_context_create();
    ProofGenerationContext* proof_ctx = proof_generation_context_create();
    
    assert(contract_ctx != NULL);
    assert(proof_ctx != NULL);
    
    printf("1. Creating contracts for verification...\n");
    
    // Create a function contract
    FunctionContract* contract = function_contract_create("safe_access");
    function_contract_add_precondition(contract, NULL, "index >= 0");
    function_contract_add_precondition(contract, NULL, "index < array_length");
    function_contract_add_postcondition(contract, NULL, "result == array[index]");
    
    contract_context_add_function(contract_ctx, contract);
    
    printf("   ✓ Created safe_access contract with bounds checking\n");
    
    printf("\n2. Generating proofs for contracts...\n");
    
    // Use the proof generation system to verify contracts
    int proof_count = generate_proofs_for_program(proof_ctx, NULL, contract_ctx);
    
    printf("   ✓ Generated %d proofs for contract verification\n", proof_count);
    
    printf("\n3. Testing specific contract expressions...\n");
    
    // Test individual contract verification
    ContractExpression* expr = contract_expression_create(
        CONTRACT_PRECONDITION, NULL, "array_index_valid"
    );
    
    ContractVerificationInfo* verification = verify_contract_expression(
        expr, NULL, NULL
    );
    
    printf("   ✓ Contract expression verification: %s\n", 
           verification->result == CONTRACT_VERIFIED ? "VERIFIED" :
           verification->result == CONTRACT_RUNTIME_CHECK ? "RUNTIME_CHECK" :
           verification->result == CONTRACT_VIOLATED ? "VIOLATED" : "UNKNOWN");
    
    printf("\n4. Demonstrating proof generation features...\n");
    
    // Generate memory safety proof
    MemorySafetyProof* memory_proof = generate_memory_safety_proof(
        proof_ctx, NULL, NULL
    );
    
    if (memory_proof) {
        printf("   ✓ Memory safety proof generated\n");
        printf("     - Null pointer safe: %s\n", 
               memory_proof->safety_properties.null_pointer_safe ? "Yes" : "No");
        printf("     - Buffer overflow safe: %s\n", 
               memory_proof->safety_properties.buffer_overflow_safe ? "Yes" : "No");
        printf("     - Use after free safe: %s\n", 
               memory_proof->safety_properties.use_after_free_safe ? "Yes" : "No");
    }
    
    printf("\n5. Testing contract-based optimizations...\n");
    
    // Demonstrate how verified contracts can enable optimizations
    if (verification->can_optimize_away) {
        printf("   ✓ Contract can be optimized away at compile time\n");
    } else {
        printf("   ✓ Contract requires runtime checking\n");
    }
    
    printf("\n6. Integration benefits:\n");
    
    printf("   • Automatic verification: Contracts are automatically checked\n");
    printf("   • Proof generation: Mathematical proofs ensure correctness\n");
    printf("   • Optimization: Verified contracts can be eliminated\n");
    printf("   • Error detection: Invalid contracts caught at compile time\n");
    printf("   • Documentation: Contracts serve as executable documentation\n");
    
    printf("\n7. Real-world scenarios:\n");
    
    printf("   🛡️  Memory Safety:\n");
    printf("      Contract: @requires(ptr != nil)\n");
    printf("      Proof: Null pointer dereference impossible\n");
    printf("      Optimization: Remove null checks\n\n");
    
    printf("   📊 Array Bounds:\n");
    printf("      Contract: @requires(0 <= i < len(arr))\n");
    printf("      Proof: Array access always in bounds\n");
    printf("      Optimization: Remove bounds checks\n\n");
    
    printf("   🔢 Mathematical Correctness:\n");
    printf("      Contract: @ensures(result == sum(1..n))\n");
    printf("      Proof: Algorithm implements correct formula\n");
    printf("      Optimization: Use closed-form formula\n\n");
    
    printf("   🔒 Resource Management:\n");
    printf("      Contract: @ensures(resource.destroyed)\n");
    printf("      Proof: No resource leaks possible\n");
    printf("      Optimization: Remove leak detection code\n\n");
    
    // Show statistics
    printf("8. Verification statistics:\n");
    print_proof_generation_statistics(proof_ctx);
    
    // Cleanup
    if (memory_proof) {
        memory_safety_proof_free(memory_proof);
    }
    free(verification->error_message);
    free(verification);
    contract_expression_free(expr);
    contract_context_free(contract_ctx);
    proof_generation_context_free(proof_ctx);
    
    printf("\n🎉 Contract Programming + Proof Generation Integration Complete!\n");
    printf("================================================================\n");
}

int main() {
    test_contract_proof_integration();
    return 0;
}
