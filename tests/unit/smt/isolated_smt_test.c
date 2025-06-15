#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../include/proof_generation.h"

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

int main() {
    printf("🧪 Simple SMT Expression Test\n");
    printf("================================\n");
    
    test_smt_expression_creation();
    
    printf("All tests passed!\n");
    return 0;
}
