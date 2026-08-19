#include "test/test_framework.h"
#include "advanced_constraint_inference.h"
#include "interface_system.h"
#include "types.h"
#include <string.h>

// =============================================================================
// Test 22.6: Advanced Constraint Inference Test Suite
// =============================================================================

// Helper function for string duplication (needed by tests)
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// Test higher-order constraint creation and management
TEST(advanced_constraint_inference, higher_order_constraints) {
    Position pos = {1, 1, 0, "test.goo"};
    
    // Create a function type for testing
    Type* param_types[] = {type_int(32, 1)};
    Type* func_type = type_function(param_types, 1, type_int(32, 1));
    
    // Test higher-order constraint creation
    HigherOrderConstraint* ho_constraint = higher_order_constraint_new(
        HO_CONSTRAINT_CALLBACK, func_type, pos);
    
    ASSERT_NOT_NULL(ho_constraint);
    ASSERT_EQ(ho_constraint->kind, HO_CONSTRAINT_CALLBACK);
    ASSERT_NOT_NULL(ho_constraint->function_type);
    ASSERT_EQ(ho_constraint->is_async, 0);
    ASSERT_EQ(ho_constraint->is_generator, 0);
    
    // Test setting callback parameters
    ho_constraint->parameter_count = 1;
    ho_constraint->parameter_types = malloc(sizeof(Type*));
    ho_constraint->parameter_types[0] = type_copy(param_types[0]);
    ho_constraint->return_type = type_copy(func_type->data.function.return_type);
    
    ASSERT_EQ(ho_constraint->parameter_count, 1);
    ASSERT_NOT_NULL(ho_constraint->parameter_types[0]);
    ASSERT_NOT_NULL(ho_constraint->return_type);
    
    // Clean up
    higher_order_constraint_free(ho_constraint);
    
    return TEST_PASS;
}

// Test variadic type pattern support
TEST(advanced_constraint_inference, variadic_type_patterns) {
    Position pos = {1, 1, 0, "test.goo"};
    
    // Create a variadic type pattern
    VariadicTypePattern* pattern = variadic_type_pattern_new(
        "...Args", TYPE_VAR_GENERIC, pos);
    
    ASSERT_NOT_NULL(pattern);
    ASSERT_NOT_NULL(pattern->name);
    ASSERT_EQ_STR(pattern->name, "...Args");
    ASSERT_EQ(pattern->element_kind, TYPE_VAR_GENERIC);
    ASSERT_EQ(pattern->min_count, 0);
    ASSERT_EQ(pattern->max_count, 0); // 0 = unlimited
    
    // Test setting bounds
    pattern->min_count = 1;
    pattern->max_count = 5;
    
    ASSERT_EQ(pattern->min_count, 1);
    ASSERT_EQ(pattern->max_count, 5);
    
    // Clean up
    variadic_type_pattern_free(pattern);
    
    return TEST_PASS;
}

// Test nested generic pattern support
TEST(advanced_constraint_inference, nested_generic_patterns) {
    Position pos = {1, 1, 0, "test.goo"};
    
    // Create types for nested generics (Vec<Option<T>>)
    Type* vec_type = type_slice(type_int(32, 1)); // Simplified as slice for now
    Type* option_type = type_nullable(type_int(32, 1)); // Simplified as nullable
    
    NestedGenericPattern* pattern = nested_generic_pattern_new(
        "VecOption", vec_type, option_type, pos);
    
    ASSERT_NOT_NULL(pattern);
    ASSERT_NOT_NULL(pattern->pattern_name);
    ASSERT_EQ_STR(pattern->pattern_name, "VecOption");
    ASSERT_EQ(pattern->nesting_depth, 2);
    ASSERT_NOT_NULL(pattern->outer_constructor);
    ASSERT_NOT_NULL(pattern->inner_constructor);
    
    // Clean up
    nested_generic_pattern_free(pattern);
    
    return TEST_PASS;
}

// Test enhanced error reporting
TEST(advanced_constraint_inference, constraint_error_reporting) {
    Position pos = {1, 1, 0, "test.goo"};
    
    // Create a constraint error
    ConstraintError* error = constraint_error_new(
        CONSTRAINT_ERROR_UNSATISFIABLE, "Type constraint cannot be satisfied", pos);
    
    ASSERT_NOT_NULL(error);
    ASSERT_EQ(error->kind, CONSTRAINT_ERROR_UNSATISFIABLE);
    ASSERT_NOT_NULL(error->primary_message);
    ASSERT_EQ_STR(error->primary_message, "Type constraint cannot be satisfied");
    ASSERT_EQ(error->confidence_score, 1.0f);
    
    // Test adding suggestions
    constraint_error_add_suggestion(error, "Try adding a trait bound");
    constraint_error_add_suggestion(error, "Consider using a different type");
    
    ASSERT_EQ(error->suggestion_count, 2);
    ASSERT_NOT_NULL(error->suggestions);
    ASSERT_EQ_STR(error->suggestions[0], "Try adding a trait bound");
    ASSERT_EQ_STR(error->suggestions[1], "Consider using a different type");
    
    // Test adding secondary positions
    Position secondary_pos = {2, 5, 10, "test.goo"};
    constraint_error_add_secondary_position(error, secondary_pos);
    
    ASSERT_EQ(error->secondary_count, 1);
    ASSERT_NOT_NULL(error->secondary_positions);
    ASSERT_EQ(error->secondary_positions[0].line, 2);
    ASSERT_EQ(error->secondary_positions[0].column, 5);
    
    // Test generating detailed error report
    char* report = generate_detailed_constraint_error_report(error);
    ASSERT_NOT_NULL(report);
    
    // Check that the report contains expected elements
    ASSERT_TRUE(strstr(report, "Constraint Error:") != NULL);
    ASSERT_TRUE(strstr(report, "Type constraint cannot be satisfied") != NULL);
    ASSERT_TRUE(strstr(report, "Try adding a trait bound") != NULL);
    ASSERT_TRUE(strstr(report, "Confidence: 1.00") != NULL);
    
    // Clean up
    free(report);
    constraint_error_free(error);
    
    return TEST_PASS;
}

// Test user-guided constraint hints
TEST(advanced_constraint_inference, constraint_hints) {
    Position pos = {1, 1, 0, "test.goo"};
    
    // Create a type annotation hint
    ConstraintHint* hint = constraint_hint_new(
        HINT_TYPE_ANNOTATION, "my_var", pos);
    
    ASSERT_NOT_NULL(hint);
    ASSERT_EQ(hint->kind, HINT_TYPE_ANNOTATION);
    ASSERT_NOT_NULL(hint->target_identifier);
    ASSERT_EQ_STR(hint->target_identifier, "my_var");
    ASSERT_EQ(hint->priority, 5); // Default priority
    ASSERT_EQ(hint->is_mandatory, 0);
    
    // Test setting suggested type
    hint->suggested_type = type_int(32, 1);
    ASSERT_NOT_NULL(hint->suggested_type);
    
    // Test trait bound hint
    ConstraintHint* trait_hint = constraint_hint_new(
        HINT_TRAIT_BOUND, "generic_param", pos);
    trait_hint->trait_name = str_dup("Display");
    trait_hint->priority = 8;
    
    ASSERT_NOT_NULL(trait_hint);
    ASSERT_EQ(trait_hint->kind, HINT_TRAIT_BOUND);
    ASSERT_NOT_NULL(trait_hint->trait_name);
    ASSERT_EQ_STR(trait_hint->trait_name, "Display");
    ASSERT_EQ(trait_hint->priority, 8);
    
    // Clean up
    constraint_hint_free(hint);
    constraint_hint_free(trait_hint);
    
    return TEST_PASS;
}

// Test advanced constraint solver
TEST(advanced_constraint_inference, advanced_constraint_solver) {
    // Create a type checker and constraint inference engine
    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker);
    
    ConstraintInferenceEngine* base_engine = constraint_inference_engine_new(checker);
    ASSERT_NOT_NULL(base_engine);
    
    // Create an advanced constraint solver
    AdvancedConstraintSolver* solver = advanced_constraint_solver_new(
        base_engine, SOLVER_STRATEGY_UNIFICATION);
    
    ASSERT_NOT_NULL(solver);
    ASSERT_EQ(solver->strategy, SOLVER_STRATEGY_UNIFICATION);
    ASSERT_EQ(solver->constraints_solved, 0);
    ASSERT_EQ(solver->unification_steps, 0);
    ASSERT_EQ(solver->backtrack_count, 0);
    
    // Test setting optimization flags
    ConstraintOptimizerFlags flags = OPT_CONSTRAINT_CACHING | OPT_EARLY_TERMINATION;
    int result = advanced_constraint_solver_set_optimization_flags(solver, flags);
    
    ASSERT_EQ(result, 1);
    ASSERT_EQ(solver->optimization_flags, flags);
    
    // Test solving (this is a basic test since we don't have complex constraints set up)
    result = advanced_constraint_solver_solve_advanced(solver);
    ASSERT_EQ(result, 1); // Should succeed with empty constraint set
    
    // Clean up
    advanced_constraint_solver_free(solver);
    constraint_inference_engine_free(base_engine);
    type_checker_free(checker);
    
    return TEST_PASS;
}

// Test language feature integration
TEST(advanced_constraint_inference, language_feature_integration) {
    Position pos = {1, 1, 0, "test.goo"};
    
    // Test error handling integration
    Type* error_union_type = type_error_union(type_int(32, 1), type_string_type());
    LanguageFeatureIntegration* integration = create_error_handling_integration(
        error_union_type, pos);
    
    ASSERT_NOT_NULL(integration);
    ASSERT_EQ(integration->context, INTEGRATION_ERROR_HANDLING);
    ASSERT_NOT_NULL(integration->primary_type);
    ASSERT_EQ(integration->primary_type->kind, TYPE_ERROR_UNION);
    
    // Test nullable integration
    Type* nullable_type = type_nullable(type_int(32, 1));
    LanguageFeatureIntegration* nullable_integration = create_nullable_integration(
        nullable_type, pos);
    
    ASSERT_NOT_NULL(nullable_integration);
    ASSERT_EQ(nullable_integration->context, INTEGRATION_NULLABLE_TYPES);
    ASSERT_NOT_NULL(nullable_integration->primary_type);
    ASSERT_EQ(nullable_integration->primary_type->kind, TYPE_NULLABLE);
    
    // Test ownership integration
    Type* owned_type = type_int(32, 1);
    LanguageFeatureIntegration* ownership_integration = create_ownership_integration(
        owned_type, OWNERSHIP_OWNED, pos);
    
    ASSERT_NOT_NULL(ownership_integration);
    ASSERT_EQ(ownership_integration->context, INTEGRATION_OWNERSHIP_SYSTEM);
    ASSERT_NOT_NULL(ownership_integration->feature_specific_data);
    
    OwnershipKind* ownership = (OwnershipKind*)ownership_integration->feature_specific_data;
    ASSERT_EQ(*ownership, OWNERSHIP_OWNED);
    
    // Test async integration
    Type* future_type = type_int(32, 1); // Simplified
    LanguageFeatureIntegration* async_integration = create_async_integration(
        future_type, pos);
    
    ASSERT_NOT_NULL(async_integration);
    ASSERT_EQ(async_integration->context, INTEGRATION_ASYNC_SYSTEM);
    
    // Test concurrency integration
    Type* channel_type = type_channel(type_int(32, 1), CHAN_PATTERN_BASIC);
    LanguageFeatureIntegration* concurrency_integration = create_concurrency_integration(
        channel_type, pos);
    
    ASSERT_NOT_NULL(concurrency_integration);
    ASSERT_EQ(concurrency_integration->context, INTEGRATION_CONCURRENCY);
    ASSERT_NOT_NULL(concurrency_integration->primary_type);
    ASSERT_EQ(concurrency_integration->primary_type->kind, TYPE_CHANNEL);
    
    // Clean up (simplified - would need proper cleanup functions)
    free(integration);
    free(nullable_integration);
    free(ownership_integration);
    free(async_integration);
    free(concurrency_integration);
    
    return TEST_PASS;
}

// Test integration with constraint inference engine
TEST(advanced_constraint_inference, constraint_inference_integration) {
    // Create a type checker and constraint inference engine
    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker);
    
    ConstraintInferenceEngine* engine = constraint_inference_engine_new(checker);
    ASSERT_NOT_NULL(engine);
    
    Position pos = {1, 1, 0, "test.goo"};
    
    // Test applying constraint hints
    ConstraintHint* hint = constraint_hint_new(HINT_TRAIT_BOUND, "T", pos);
    hint->trait_name = str_dup("Display");
    
    int result = apply_constraint_hint(engine, hint);
    ASSERT_EQ(result, 1);
    
    // Check that the constraint was added
    ASSERT_TRUE(engine->active_constraints->count > 0);
    
    // Test error handling integration
    Type* error_union_type = type_error_union(type_int(32, 1), type_string_type());
    LanguageFeatureIntegration* integration = create_error_handling_integration(
        error_union_type, pos);
    
    result = integrate_with_error_handling(engine, integration);
    ASSERT_EQ(result, 1);
    
    // Test nullable integration
    Type* nullable_type = type_nullable(type_int(32, 1));
    LanguageFeatureIntegration* nullable_integration = create_nullable_integration(
        nullable_type, pos);
    
    result = integrate_with_nullable_types(engine, nullable_integration);
    ASSERT_EQ(result, 1);
    
    // Test ownership integration
    Type* owned_type = type_int(32, 1);
    LanguageFeatureIntegration* ownership_integration = create_ownership_integration(
        owned_type, OWNERSHIP_OWNED, pos);
    
    result = integrate_with_ownership_system(engine, ownership_integration);
    ASSERT_EQ(result, 1);
    
    // Test async integration
    Type* future_type = type_int(32, 1);
    LanguageFeatureIntegration* async_integration = create_async_integration(
        future_type, pos);
    
    result = integrate_with_async_system(engine, async_integration);
    ASSERT_EQ(result, 1);
    
    // Test concurrency integration
    Type* channel_type = type_channel(type_int(32, 1), CHAN_PATTERN_BASIC);
    LanguageFeatureIntegration* concurrency_integration = create_concurrency_integration(
        channel_type, pos);
    
    result = integrate_with_concurrency(engine, concurrency_integration);
    ASSERT_EQ(result, 1);
    
    // Verify that constraints were added
    ASSERT_TRUE(engine->constraints_inferred > 0);
    
    // Clean up
    constraint_hint_free(hint);
    free(integration);
    free(nullable_integration);
    free(ownership_integration);
    free(async_integration);
    free(concurrency_integration);
    constraint_inference_engine_free(engine);
    type_checker_free(checker);
    
    return TEST_PASS;
}
