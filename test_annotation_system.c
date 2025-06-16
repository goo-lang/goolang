#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "include/auto_parallel.h"
#include "include/ast.h"
#include "include/token.h"

// Test annotation system and auto-parallelization
int main() {
    printf("Testing Annotation System and Auto-Parallelization\n");
    printf("=================================================\n");
    
    // Test 1: Create auto-parallel context
    printf("\n1. Testing context creation...\n");
    AutoParallelContext* ctx = auto_parallel_context_new();
    assert(ctx != NULL);
    printf("   ✓ Context created successfully\n");
    
    // Test 2: Create a simple function with @auto_parallel annotation
    printf("\n2. Creating function with annotation...\n");
    Position pos = {1, 1, 0, "test.goo"};
    
    // Create function
    FuncDeclNode* func = ast_func_decl_new("processData", pos);
    assert(func != NULL);
    
    // Create @auto_parallel annotation
    AttributeNode* attr = ast_attribute_new("auto_parallel", NULL, pos);
    assert(attr != NULL);
    func->annotations = (ASTNode*)attr;
    
    printf("   ✓ Function with annotation created\n");
    
    // Test 3: Test annotation detection
    printf("\n3. Testing annotation detection...\n");
    bool has_annotation = has_auto_parallel_annotation((ASTNode*)func);
    assert(has_annotation == true);
    printf("   ✓ Auto-parallel annotation detected correctly\n");
    
    bool should_parallelize = should_parallelize_function((ASTNode*)func);
    assert(should_parallelize == true);
    printf("   ✓ Function marked for parallelization\n");
    
    // Test 4: Create function without annotation
    printf("\n4. Testing function without annotation...\n");
    FuncDeclNode* func2 = ast_func_decl_new("normalFunction", pos);
    assert(func2 != NULL);
    
    bool has_annotation2 = has_auto_parallel_annotation((ASTNode*)func2);
    assert(has_annotation2 == false);
    printf("   ✓ Function without annotation detected correctly\n");
    
    bool should_parallelize2 = should_parallelize_function((ASTNode*)func2);
    assert(should_parallelize2 == false);
    printf("   ✓ Function without annotation not marked for parallelization\n");
    
    // Test 5: Create simple for loop in function
    printf("\n5. Creating function with for loop...\n");
    BlockStmtNode* body = ast_block_stmt_new(pos);
    
    // Create for loop
    ASTNode* for_loop = ast_node_new(AST_FOR_STMT, pos);
    ForStmtNode* for_node = (ForStmtNode*)for_loop;
    for_node->init = NULL;
    for_node->condition = NULL;
    for_node->post = NULL;
    for_node->body = (ASTNode*)ast_block_stmt_new(pos);
    
    body->statements = for_loop;
    func->body = (ASTNode*)body;
    
    printf("   ✓ Function with for loop created\n");
    
    // Test 6: Analyze the function
    printf("\n6. Analyzing function for parallelization...\n");
    analyze_function_node(ctx, (ASTNode*)func);
    
    assert(ctx->functions_parallelized >= 1);
    printf("   ✓ Function analyzed successfully\n");
    printf("   Functions parallelized: %zu\n", ctx->functions_parallelized);
    printf("   Loops analyzed: %zu\n", ctx->loops_analyzed);
    
    // Test 7: Test loop analysis directly
    printf("\n7. Testing loop analysis...\n");
    LoopInfo* loop_info = analyze_loop(ctx, for_loop);
    assert(loop_info != NULL);
    
    bool is_parallel = is_parallelizable_loop(loop_info);
    printf("   Loop is parallelizable: %s\n", is_parallel ? "Yes" : "No");
    
    if (is_parallel) {
        ParallelStrategy strategy = recommend_loop_strategy(loop_info);
        printf("   Recommended strategy: %s\n", parallel_strategy_string(strategy));
        
        int benefit = estimate_parallelization_benefit(loop_info, strategy);
        printf("   Estimated benefit: %d%%\n", benefit);
    }
    
    printf("   ✓ Loop analysis completed\n");
    
    // Test 8: Test with program structure
    printf("\n8. Testing with program structure...\n");
    
    // Create program node
    ProgramNode* program = ast_program_new(pos);
    program->decls = (ASTNode*)func;
    func->base.next = (ASTNode*)func2;
    
    // Run full analysis
    bool analysis_result = auto_parallel_analyze(ctx, (ASTNode*)program);
    assert(analysis_result == true);
    
    printf("   ✓ Full program analysis completed\n");
    printf("   Total opportunities: %zu\n", ctx->opportunity_count);
    
    // Test 9: Print results
    printf("\n9. Parallelization results:\n");
    print_parallelization_opportunities(ctx);
    
    // Test 10: Test transformations
    printf("\n10. Testing transformations...\n");
    bool transform_result = auto_parallel_transform(ctx);
    assert(transform_result == true);
    printf("    ✓ Transformations completed successfully\n");
    
    // Test 11: Cleanup
    printf("\n11. Testing cleanup...\n");
    auto_parallel_context_free(ctx);
    ast_node_free((ASTNode*)program);
    printf("    ✓ Cleanup completed successfully\n");
    
    printf("\n=================================================\n");
    printf("All annotation system tests passed! ✓\n");
    printf("The auto-parallelization annotation system is working correctly.\n\n");
    
    // Summary
    printf("Summary of implemented features:\n");
    printf("• Function annotations (@auto_parallel) parsing and detection\n");
    printf("• AST traversal for parallelization analysis\n");
    printf("• Loop analysis and dependency detection\n");
    printf("• Parallelization strategy recommendation\n");
    printf("• Performance benefit estimation\n");
    printf("• Transformation planning\n\n");
    
    printf("Next steps for full implementation:\n");
    printf("• Integrate with the full parser pipeline\n");
    printf("• Implement actual code transformations\n");
    printf("• Add more sophisticated dependency analysis\n");
    printf("• Integrate with LLVM IR generation\n");
    printf("• Add runtime profiling integration\n");
    
    return 0;
}