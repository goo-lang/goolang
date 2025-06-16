#include "profile_guided_optimization.h"
#include "optimization.h"
#include "comptime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Test helper functions
void test_profile_data_management(void);
void test_profile_collector(void);
void test_profile_analysis(void);
void test_pgo_optimization_generation(void);
void test_pgo_compile_time_integration(void);
void test_pgo_intrinsics(void);
void test_profile_utilities(void);

void test_profile_data_management(void) {
    printf("Testing profile data management...\n");
    
    // Test profile data creation
    ProfileData* data = profile_data_new("test_program.c");
    assert(data != NULL);
    assert(strcmp(data->source_file, "test_program.c") == 0);
    assert(data->functions == NULL);
    assert(data->loops == NULL);
    assert(data->call_sites == NULL);
    assert(data->total_samples == 0);
    
    // Test adding function profiles
    int result = profile_add_function(data, "main", 1, 1000000);
    assert(result == 1);
    assert(data->functions != NULL);
    assert(strcmp(data->functions->function_name, "main") == 0);
    assert(data->functions->call_count == 1);
    assert(data->functions->total_cycles == 1000000);
    
    result = profile_add_function(data, "helper", 100, 500000);
    assert(result == 1);
    
    // Test adding branch information
    result = profile_add_branch(data, "main.c:25:10", 800, 200);
    assert(result == 1);
    assert(data->functions->branches != NULL);
    
    // Test adding loop information
    result = profile_add_loop(data, "main.c:30:5", 10000, 100);
    assert(result == 1);
    assert(data->loops != NULL);
    assert(data->loops->iteration_count == 10000);
    assert(data->loops->invocation_count == 100);
    assert(data->loops->average_iterations == 100.0);
    
    // Test adding call site information
    result = profile_add_call_site(data, "main.c:35:8", "helper", 100);
    assert(result == 1);
    assert(data->call_sites != NULL);
    assert(strcmp(data->call_sites->target_function, "helper") == 0);
    
    // Test hotness calculation
    profile_calculate_hotness_scores(data);
    assert(data->functions->hotness_score >= 0.0);
    assert(data->functions->hotness_score <= 1.0);
    
    // Test branch probability calculation
    profile_calculate_branch_probabilities(data);
    assert(data->functions->branches->taken_probability == 0.8); // 800/(800+200)
    
    // Test hot/cold function identification
    profile_identify_hot_cold_functions(data, 0.5, 0.1);
    
    profile_data_free(data);
    
    printf("✓ Profile data management tests passed!\n");
}

void test_profile_collector(void) {
    printf("Testing profile collector...\n");
    
    // Test collector creation
    ProfileCollector* collector = profile_collector_new(PROFILE_MODE_SAMPLING);
    assert(collector != NULL);
    assert(collector->mode == PROFILE_MODE_SAMPLING);
    assert(collector->sampling_rate == 1000.0);
    assert(collector->collect_branches == true);
    assert(collector->is_collecting == false);
    
    // Test starting collection
    int result = profile_collector_start(collector, "test_program");
    assert(result == 1);
    assert(collector->is_collecting == true);
    assert(collector->current_data != NULL);
    
    // Test stopping collection
    result = profile_collector_stop(collector);
    assert(result == 1);
    assert(collector->is_collecting == false);
    assert(collector->current_data->collection_time >= 0.0); // Allow 0.0 for fast execution
    
    // Verify simulated data was collected
    assert(collector->current_data->functions != NULL);
    assert(collector->current_data->total_samples > 0);
    
    profile_collector_free(collector);
    
    printf("✓ Profile collector tests passed!\n");
}

void test_profile_analysis(void) {
    printf("Testing profile analysis...\n");
    
    ProfileData* data = profile_data_new("analysis_test.c");
    
    // Add test data
    profile_add_function(data, "hot_function", 1000, 10000000);
    profile_add_function(data, "cold_function", 1, 100);
    profile_add_function(data, "warm_function", 100, 1000000);
    
    profile_add_branch(data, "test.c:10:5", 900, 100);
    profile_add_branch(data, "test.c:20:8", 100, 900);
    profile_add_branch(data, "test.c:30:12", 500, 500);
    
    // Calculate metrics
    profile_calculate_hotness_scores(data);
    profile_calculate_branch_probabilities(data);
    profile_identify_hot_cold_functions(data, 0.5, 0.1);
    
    // Test hotness analysis
    double hotness = profile_function_hotness(data, "hot_function");
    assert(hotness > 0.5);
    
    hotness = profile_function_hotness(data, "cold_function");
    assert(hotness < 0.1);
    
    // Test branch probability analysis
    double prob = profile_branch_probability(data, "test.c:10:5");
    assert(prob == 0.9); // 900/(900+100)
    
    prob = profile_branch_probability(data, "test.c:20:8");
    assert(prob == 0.1); // 100/(100+900)
    
    prob = profile_branch_probability(data, "test.c:30:12");
    assert(prob == 0.5); // 500/(500+500)
    
    // Test hot/cold function identification
    bool is_hot = profile_is_function_hot(data, "hot_function", 0.5);
    assert(is_hot == true);
    
    bool is_cold = profile_is_function_cold(data, "cold_function", 0.1);
    assert(is_cold == true);
    
    profile_data_free(data);
    
    printf("✓ Profile analysis tests passed!\n");
}

void test_pgo_optimization_generation(void) {
    printf("Testing PGO optimization generation...\n");
    
    // Create profile data
    ProfileData* data = profile_data_new("pgo_test.c");
    profile_add_function(data, "hot_function", 1000, 10000000);
    profile_add_function(data, "cold_function", 1, 100);
    profile_add_branch(data, "test.c:15:5", 950, 50);
    profile_calculate_hotness_scores(data);
    profile_calculate_branch_probabilities(data);
    
    // Create optimization context
    ComptimeContext* comptime_ctx = comptime_context_new(NULL);
    OptimizationContext* opt_ctx = optimization_context_new(comptime_ctx);
    PGOContext* pgo_ctx = pgo_context_new(data, opt_ctx);
    
    assert(pgo_ctx != NULL);
    assert(pgo_ctx->profile_data == data);
    assert(pgo_ctx->confidence_threshold == 0.8);
    
    // Create mock AST nodes for testing
    ASTNode* func_node = malloc(sizeof(ASTNode));
    func_node->type = AST_FUNC_DECL;
    func_node->pos.line = 1;
    func_node->pos.column = 1;
    
    ASTNode* branch_node = malloc(sizeof(ASTNode));
    branch_node->type = AST_IF_STMT;
    branch_node->pos.line = 15;
    branch_node->pos.column = 5;
    
    // Test function optimization
    ComptimeResult* result = pgo_optimize_function(pgo_ctx, "hot_function", func_node);
    assert(result != NULL);
    assert(result->error == NULL);
    assert(result->value != NULL);
    assert(strstr(result->value->string_value, "Hot function") != NULL);
    assert(strstr(result->value->string_value, "O3") != NULL);
    comptime_result_free(result);
    
    result = pgo_optimize_function(pgo_ctx, "cold_function", func_node);
    assert(result != NULL);
    assert(result->error == NULL);
    assert(strstr(result->value->string_value, "Cold function") != NULL);
    assert(strstr(result->value->string_value, "Os") != NULL);
    comptime_result_free(result);
    
    // Test branch optimization
    result = pgo_optimize_branch(pgo_ctx, "test.c:15:5", branch_node);
    assert(result != NULL);
    assert(result->error == NULL);
    assert(strstr(result->value->string_value, "likely") != NULL);
    assert(strstr(result->value->string_value, "__builtin_expect") != NULL);
    comptime_result_free(result);
    
    // Clean up
    free(func_node);
    free(branch_node);
    pgo_context_free(pgo_ctx);
    optimization_context_free(opt_ctx);
    comptime_context_free(comptime_ctx);
    profile_data_free(data);
    
    printf("✓ PGO optimization generation tests passed!\n");
}

void test_pgo_compile_time_integration(void) {
    printf("Testing PGO compile-time integration...\n");
    
    ComptimeContext* ctx = comptime_context_new(NULL);
    
    // Test profile analysis integration
    ComptimeResult* result = comptime_pgo_analyze(ctx, "test_profile.pgo");
    assert(result != NULL);
    assert(result->error == NULL);
    assert(result->value != NULL);
    assert(strstr(result->value->string_value, "PGO Analysis Results") != NULL);
    assert(strstr(result->value->string_value, "test_profile.pgo") != NULL);
    
    printf("PGO Analysis Output:\n%s\n", result->value->string_value);
    comptime_result_free(result);
    
    comptime_context_free(ctx);
    
    printf("✓ PGO compile-time integration tests passed!\n");
}

void test_pgo_intrinsics(void) {
    printf("Testing PGO intrinsics...\n");
    
    ComptimeContext* ctx = comptime_context_new(NULL);
    
    // Test profile hotness intrinsic
    ComptimeValue function_name;
    function_name.type = COMPTIME_VALUE_STRING;
    function_name.string_value = "process_data";
    
    ComptimeResult* result = comptime_intrinsic_profile_hotness(ctx, &function_name);
    assert(result != NULL);
    assert(result->error == NULL);
    assert(result->value != NULL);
    assert(result->value->type == COMPTIME_VALUE_FLOAT);
    assert(result->value->float_value >= 0.0);
    assert(result->value->float_value <= 1.0);
    
    printf("Hotness for 'process_data': %.2f\n", result->value->float_value);
    comptime_result_free(result);
    
    // Test with different function names
    function_name.string_value = "main";
    result = comptime_intrinsic_profile_hotness(ctx, &function_name);
    assert(result != NULL);
    assert(result->error == NULL);
    printf("Hotness for 'main': %.2f\n", result->value->float_value);
    comptime_result_free(result);
    
    function_name.string_value = "unknown_function";
    result = comptime_intrinsic_profile_hotness(ctx, &function_name);
    assert(result != NULL);
    assert(result->error == NULL);
    printf("Hotness for 'unknown_function': %.2f\n", result->value->float_value);
    comptime_result_free(result);
    
    comptime_context_free(ctx);
    
    printf("✓ PGO intrinsics tests passed!\n");
}

void test_profile_utilities(void) {
    printf("Testing profile utilities...\n");
    
    ProfileData* data = profile_data_new("utilities_test.c");
    
    // Add comprehensive test data
    profile_add_function(data, "main", 1, 500000);
    profile_add_function(data, "process_data", 1000, 8000000);
    profile_add_function(data, "sort_array", 100, 3000000);
    profile_add_function(data, "helper", 10000, 1000000);
    profile_add_function(data, "rare_function", 1, 1000);
    
    profile_calculate_hotness_scores(data);
    profile_identify_hot_cold_functions(data, 0.3, 0.05);
    
    // Test summary printing (visual verification)
    printf("\n--- Profile Summary ---\n");
    profile_print_summary(data);
    printf("--- End Summary ---\n\n");
    
    // Verify hot/cold function identification
    assert(data->hot_function_count > 0);
    assert(data->cold_function_count > 0);
    
    bool found_hot_process_data = false;
    for (size_t i = 0; i < data->hot_function_count; i++) {
        if (strcmp(data->hot_functions[i], "process_data") == 0) {
            found_hot_process_data = true;
            break;
        }
    }
    assert(found_hot_process_data == true);
    
    profile_data_free(data);
    
    printf("✓ Profile utilities tests passed!\n");
}

int main(void) {
    printf("Running Profile-Guided Optimization tests...\n\n");
    
    test_profile_data_management();
    test_profile_collector();
    test_profile_analysis();
    test_pgo_optimization_generation();
    test_pgo_compile_time_integration();
    test_pgo_intrinsics();
    test_profile_utilities();
    
    printf("\n✅ All Profile-Guided Optimization tests passed!\n");
    return 0;
}
