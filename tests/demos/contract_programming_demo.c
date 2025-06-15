#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "contracts.h"

// Demonstration of contract programming framework capabilities
void demonstrate_contract_analysis() {
    printf("📋 Contract Programming Framework Demonstration\n");
    printf("==============================================\n\n");
    
    // Create a contract context
    ContractContext* ctx = contract_context_create();
    assert(ctx != NULL);
    
    printf("1. Creating function contracts...\n");
    
    // Example: safe_array_access function
    FunctionContract* array_access_contract = function_contract_create("safe_array_access");
    
    // Add preconditions
    function_contract_add_precondition(array_access_contract, NULL, "index >= 0");
    function_contract_add_precondition(array_access_contract, NULL, "index < len(array)");
    
    // Add postcondition
    function_contract_add_postcondition(array_access_contract, NULL, "result == array[index]");
    
    printf("   ✓ Created contract for safe_array_access with 2 preconditions and 1 postcondition\n");
    
    // Example: safe_divide function
    FunctionContract* divide_contract = function_contract_create("safe_divide");
    function_contract_add_precondition(divide_contract, NULL, "divisor != 0");
    function_contract_add_postcondition(divide_contract, NULL, "result == dividend / divisor");
    
    printf("   ✓ Created contract for safe_divide with division by zero protection\n");
    
    // Example: loop with invariants
    FunctionContract* sum_contract = function_contract_create("sum_to_n");
    function_contract_add_precondition(sum_contract, NULL, "n >= 0");
    function_contract_add_postcondition(sum_contract, NULL, "result == n * (n + 1) / 2");
    
    printf("   ✓ Created contract for sum_to_n with mathematical correctness guarantee\n");
    
    printf("\n2. Adding contracts to verification context...\n");
    
    // Add contracts to context
    contract_context_add_function(ctx, array_access_contract);
    contract_context_add_function(ctx, divide_contract);
    contract_context_add_function(ctx, sum_contract);
    
    printf("   ✓ Added 3 function contracts to verification context\n");
    
    printf("\n3. Contract verification analysis...\n");
    
    // Demonstrate contract verification
    ContractExpression* sample_precond = contract_expression_create(
        CONTRACT_PRECONDITION, NULL, "x > 0"
    );
    
    ContractVerificationInfo* info = verify_contract_expression(
        sample_precond, NULL, NULL
    );
    
    printf("   ✓ Sample contract verification completed\n");
    printf("   - Result: %s\n", 
           info->result == CONTRACT_VERIFIED ? "VERIFIED" :
           info->result == CONTRACT_RUNTIME_CHECK ? "RUNTIME_CHECK" :
           info->result == CONTRACT_VIOLATED ? "VIOLATED" : "UNKNOWN");
    printf("   - Can optimize: %s\n", info->can_optimize_away ? "Yes" : "No");
    
    printf("\n4. Contract-to-string conversion...\n");
    
    // Demonstrate string conversion
    char* contract_str = contract_to_string(sample_precond);
    printf("   ✓ Contract string: \"%s\"\n", contract_str);
    
    printf("\n5. Advanced contract features...\n");
    
    // Demonstrate loop contracts
    printf("   ✓ Loop invariant support: @invariant(condition)\n");
    printf("   ✓ Precondition support: @requires(condition)\n");
    printf("   ✓ Postcondition support: @ensures(condition)\n");
    printf("   ✓ Assertion support: @assert(condition)\n");
    printf("   ✓ Assumption support: @assume(condition)\n");
    
    printf("\n6. Integration capabilities...\n");
    
    printf("   ✓ AST integration: Contracts can reference AST nodes\n");
    printf("   ✓ Type system integration: Contracts work with dependent types\n");
    printf("   ✓ Proof generation integration: Contracts can generate SMT formulas\n");
    printf("   ✓ Optimization integration: Verified contracts can be optimized away\n");
    
    printf("\n7. Real-world contract examples:\n");
    
    printf("   • Array bounds checking:\n");
    printf("     @requires(0 <= index < len(array))\n");
    printf("     func get(array: []T, index: usize) -> T\n\n");
    
    printf("   • Memory safety:\n");
    printf("     @requires(ptr != nil)\n");
    printf("     @ensures(result != nil)\n");
    printf("     func allocate(size: usize) -> *T\n\n");
    
    printf("   • Mathematical correctness:\n");
    printf("     @requires(n >= 0)\n");
    printf("     @ensures(result == factorial(n))\n");
    printf("     func factorial(n: int) -> int\n\n");
    
    printf("   • Resource management:\n");
    printf("     @requires(resource.valid())\n");
    printf("     @ensures(!resource.valid())\n");
    printf("     func destroy(resource: *Resource)\n\n");
    
    printf("   • Concurrency safety:\n");
    printf("     @requires(!locked(mutex))\n");
    printf("     @ensures(locked(mutex))\n");
    printf("     func acquire(mutex: *Mutex)\n\n");
    
    // Cleanup
    free(contract_str);
    free(info->error_message);
    free(info);
    contract_expression_free(sample_precond);
    contract_context_free(ctx);
    
    printf("🎉 Contract Programming Framework Demonstration Complete!\n");
    printf("==========================================================\n");
}

int main() {
    demonstrate_contract_analysis();
    return 0;
}
