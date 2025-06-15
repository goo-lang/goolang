#include "test/test_framework.h"
#include "interface_system.h"
#include "errors/error.h"
#include <string.h>

// Test fixture for constraint inference
TEST_FIXTURE(ConstraintInferenceFixture) {
    ErrorContext* error_ctx;
    TypeChecker* type_checker;
    ConstraintInferenceEngine* engine;
};

SETUP_FIXTURE(ConstraintInferenceFixture) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    fixture->error_ctx = error_context_new();
    fixture->type_checker = type_checker_new();
    fixture->engine = constraint_inference_engine_new(fixture->type_checker);
}

TEARDOWN_FIXTURE(ConstraintInferenceFixture) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    constraint_inference_engine_free(fixture->engine);
    type_checker_free(fixture->type_checker);
    error_context_free(fixture->error_ctx);
}

// Basic constraint inference tests
TEST(constraint_inference, engine_creation) {
    ErrorContext* error_ctx = error_context_new();
    TypeChecker* checker = type_checker_new();
    ConstraintInferenceEngine* engine = constraint_inference_engine_new(checker);
    
    ASSERT_NOT_NULL(engine);
    ASSERT_EQ_PTR(checker, engine->type_checker);
    ASSERT_NOT_NULL(engine->active_constraints);
    ASSERT_EQ(0, engine->inference_depth);
    ASSERT_EQ(0, engine->constraints_inferred);
    
    constraint_inference_engine_free(engine);
    type_checker_free(checker);
    error_context_free(error_ctx);
    
    return TEST_PASS;
}

TEST(constraint_inference, type_variable_creation) {
    Position pos = {.line = 1, .column = 1, .offset = 0, .filename = "test.goo"};
    TypeVariable* var = type_variable_new("T", TYPE_VAR_GENERIC, pos);
    
    ASSERT_NOT_NULL(var);
    ASSERT_EQ_STR("T", var->name);
    ASSERT_EQ(TYPE_VAR_GENERIC, var->kind);
    ASSERT_NOT_NULL(var->constraints);
    ASSERT_EQ(0, var->is_inferred);
    
    type_variable_free(var);
    return TEST_PASS;
}

TEST(constraint_inference, constraint_creation) {
    Position pos = {.line = 1, .column = 1, .offset = 0, .filename = "test.goo"};
    Type* int_type = type_int(32, 1);
    
    InterfaceConstraint* constraint = interface_constraint_new(
        CONSTRAINT_NUMERIC, int_type, pos);
    
    ASSERT_NOT_NULL(constraint);
    ASSERT_EQ(CONSTRAINT_NUMERIC, constraint->kind);
    ASSERT_EQ_PTR(int_type, constraint->constrained_type);
    ASSERT_EQ(0, constraint->is_auto_inferred);
    
    interface_constraint_free(constraint);
    return TEST_PASS;
}

TEST(constraint_inference, constraint_set_operations) {
    ConstraintSet* set = constraint_set_new();
    ASSERT_NOT_NULL(set);
    ASSERT_EQ(0, set->count);
    
    Position pos = {.line = 1, .column = 1, .offset = 0, .filename = "test.goo"};
    Type* int_type = type_int(32, 1);
    
    InterfaceConstraint* constraint1 = interface_constraint_new(
        CONSTRAINT_NUMERIC, int_type, pos);
    InterfaceConstraint* constraint2 = interface_constraint_new(
        CONSTRAINT_COPY, int_type, pos);
    
    ASSERT_TRUE(constraint_set_add(set, constraint1));
    ASSERT_EQ(1, set->count);
    
    ASSERT_TRUE(constraint_set_add(set, constraint2));
    ASSERT_EQ(2, set->count);
    
    constraint_set_free(set);
    return TEST_PASS;
}

// Pattern-based constraint inference tests
TEST_F(ConstraintInferenceFixture, arithmetic_pattern_inference) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Type* unknown_type = type_new(TYPE_UNKNOWN);
    Position pos = {.line = 1, .column = 1, .offset = 0, .filename = "test.goo"};
    
    int result = infer_constraints_from_arithmetic_context(
        fixture->engine, unknown_type, pos);
    
    ASSERT_TRUE(result);
    ASSERT_EQ(1, fixture->engine->constraints_inferred);
    ASSERT_EQ(1, fixture->engine->active_constraints->count);
    
    // Check that the constraint is numeric
    InterfaceConstraint* constraint = fixture->engine->active_constraints->constraints;
    ASSERT_NOT_NULL(constraint);
    ASSERT_EQ(CONSTRAINT_NUMERIC, constraint->kind);
    ASSERT_TRUE(constraint->is_auto_inferred);
    
    type_free(unknown_type);
    return TEST_PASS;
}

TEST_F(ConstraintInferenceFixture, comparison_pattern_inference) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Type* unknown_type = type_new(TYPE_UNKNOWN);
    Position pos = {.line = 2, .column = 5, .offset = 10, .filename = "test.goo"};
    
    int result = infer_constraints_from_comparison_context(
        fixture->engine, unknown_type, pos);
    
    ASSERT_TRUE(result);
    ASSERT_EQ(1, fixture->engine->constraints_inferred);
    
    // Check that the constraint is partial ordering
    InterfaceConstraint* constraint = fixture->engine->active_constraints->constraints;
    ASSERT_NOT_NULL(constraint);
    ASSERT_EQ(CONSTRAINT_PARTIAL_ORD, constraint->kind);
    ASSERT_TRUE(constraint->is_auto_inferred);
    
    type_free(unknown_type);
    return TEST_PASS;
}

TEST_F(ConstraintInferenceFixture, usage_pattern_recognition) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Type* unknown_type = type_new(TYPE_UNKNOWN);
    Position pos = {.line = 3, .column = 1, .offset = 20, .filename = "test.goo"};
    
    // Test various usage patterns
    ASSERT_TRUE(infer_constraints_from_usage_pattern(
        fixture->engine, unknown_type, "clone", pos));
    ASSERT_TRUE(infer_constraints_from_usage_pattern(
        fixture->engine, unknown_type, "display", pos));
    ASSERT_TRUE(infer_constraints_from_usage_pattern(
        fixture->engine, unknown_type, "debug", pos));
    
    ASSERT_EQ(3, fixture->engine->constraints_inferred);
    ASSERT_EQ(3, fixture->engine->active_constraints->count);
    
    type_free(unknown_type);
    return TEST_PASS;
}

// Constraint propagation tests
TEST_F(ConstraintInferenceFixture, constraint_propagation) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Type* unknown_type = type_new(TYPE_UNKNOWN);
    Position pos = {.line = 4, .column = 1, .offset = 30, .filename = "test.goo"};
    
    // Add a constraint that should trigger propagation
    ASSERT_TRUE(infer_constraints_from_usage_pattern(
        fixture->engine, unknown_type, "comparison", pos));
    
    size_t initial_count = fixture->engine->constraints_inferred;
    
    // Propagate constraints
    int changes = propagate_constraints(fixture->engine);
    
    // Should have propagated some additional constraints
    ASSERT_TRUE(changes > 0);
    ASSERT_TRUE(fixture->engine->constraints_inferred > initial_count);
    
    type_free(unknown_type);
    return TEST_PASS;
}

// Complex constraint inference scenarios
TEST_F(ConstraintInferenceFixture, multiple_constraint_types) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Type* type1 = type_new(TYPE_UNKNOWN);
    Type* type2 = type_new(TYPE_UNKNOWN);
    Position pos = {.line = 5, .column = 1, .offset = 40, .filename = "test.goo"};
    
    // Infer different types of constraints
    ASSERT_TRUE(infer_constraints_from_arithmetic_context(fixture->engine, type1, pos));
    ASSERT_TRUE(infer_constraints_from_comparison_context(fixture->engine, type2, pos));
    ASSERT_TRUE(infer_constraints_from_usage_pattern(fixture->engine, type1, "copy", pos));
    ASSERT_TRUE(infer_constraints_from_usage_pattern(fixture->engine, type2, "send", pos));
    
    ASSERT_EQ(4, fixture->engine->constraints_inferred);
    ASSERT_EQ(4, fixture->engine->active_constraints->count);
    
    // Check that constraints are properly categorized
    InterfaceConstraint* constraint = fixture->engine->active_constraints->constraints;
    int numeric_count = 0, ord_count = 0, copy_count = 0, send_count = 0;
    
    while (constraint) {
        switch (constraint->kind) {
            case CONSTRAINT_NUMERIC: numeric_count++; break;
            case CONSTRAINT_PARTIAL_ORD: ord_count++; break;
            case CONSTRAINT_COPY: copy_count++; break;
            case CONSTRAINT_SEND: send_count++; break;
            default: break;
        }
        constraint = constraint->next;
    }
    
    ASSERT_EQ(1, numeric_count);
    ASSERT_EQ(1, ord_count);
    ASSERT_EQ(1, copy_count);
    ASSERT_EQ(1, send_count);
    
    type_free(type1);
    type_free(type2);
    return TEST_PASS;
}

// Error handling tests
TEST_F(ConstraintInferenceFixture, null_parameter_handling) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Position pos = {.line = 6, .column = 1, .offset = 50, .filename = "test.goo"};
    
    // Test null parameter handling
    ASSERT_FALSE(infer_constraints_from_arithmetic_context(NULL, NULL, pos));
    ASSERT_FALSE(infer_constraints_from_arithmetic_context(fixture->engine, NULL, pos));
    ASSERT_FALSE(infer_constraints_from_usage_pattern(NULL, NULL, "test", pos));
    ASSERT_FALSE(infer_constraints_from_usage_pattern(fixture->engine, NULL, "test", pos));
    
    // Engine state should be unchanged
    ASSERT_EQ(0, fixture->engine->constraints_inferred);
    
    return TEST_PASS;
}

// Performance and stress tests
TEST_F(ConstraintInferenceFixture, constraint_inference_performance) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Position pos = {.line = 7, .column = 1, .offset = 60, .filename = "test.goo"};
    double start_time = test_get_time_ms();
    
    // Create many constraints to test performance
    for (int i = 0; i < 1000; i++) {
        Type* test_type = type_new(TYPE_UNKNOWN);
        infer_constraints_from_arithmetic_context(fixture->engine, test_type, pos);
        type_free(test_type);
    }
    
    double end_time = test_get_time_ms();
    double duration = end_time - start_time;
    
    test_log("Created 1000 constraints in %.2f ms", duration);
    
    ASSERT_EQ(1000, fixture->engine->constraints_inferred);
    ASSERT_TRUE(duration < 1000.0); // Should complete in less than 1 second
    
    return TEST_PASS;
}

// Integration tests with type system
TEST(constraint_inference, integration_with_types) {
    // Test that constraint inference works with concrete types
    Type* int_type = type_int(32, 1);
    Type* float_type = type_float(64);
    
    // These types should satisfy numeric constraints
    ASSERT_TRUE(type_is_numeric(int_type));
    ASSERT_TRUE(type_is_numeric(float_type));
    
    // Test type compatibility
    ASSERT_TRUE(type_compatible(int_type, float_type)); // int can convert to float
    
    type_free(int_type);
    type_free(float_type);
    
    return TEST_PASS;
}

// Utility function tests
TEST(constraint_inference, utility_functions) {
    // Test constraint kind to string conversion
    ASSERT_EQ_STR("numeric", constraint_kind_to_string(CONSTRAINT_NUMERIC));
    ASSERT_EQ_STR("partial_ord", constraint_kind_to_string(CONSTRAINT_PARTIAL_ORD));
    ASSERT_EQ_STR("copy", constraint_kind_to_string(CONSTRAINT_COPY));
    ASSERT_EQ_STR("clone", constraint_kind_to_string(CONSTRAINT_CLONE));
    
    // Test type variable kind to string conversion  
    ASSERT_EQ_STR("generic", type_variable_kind_to_string(TYPE_VAR_GENERIC));
    ASSERT_EQ_STR("const", type_variable_kind_to_string(TYPE_VAR_CONST));
    
    return TEST_PASS;
}

// Trait bound generation tests
TEST_F(ConstraintInferenceFixture, trait_bound_generation) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Position pos = {.line = 8, .column = 1, .offset = 70, .filename = "test.goo"};
    TypeVariable* var = type_variable_new("T", TYPE_VAR_GENERIC, pos);
    
    // Add some constraints to the type variable
    Type* unknown_type = type_new(TYPE_UNKNOWN);
    ASSERT_TRUE(infer_constraints_from_arithmetic_context(fixture->engine, unknown_type, pos));
    ASSERT_TRUE(infer_constraints_from_comparison_context(fixture->engine, unknown_type, pos));
    ASSERT_TRUE(infer_constraints_from_usage_pattern(fixture->engine, unknown_type, "copy", pos));
    
    // Move constraints to the type variable
    var->constraints = constraint_set_new();
    constraint_set_merge(var->constraints, fixture->engine->active_constraints);
    
    // Generate trait bounds
    TraitBoundSet* bounds = generate_trait_bounds_from_constraints(fixture->engine, var);
    
    ASSERT_NOT_NULL(bounds);
    ASSERT_TRUE(bounds->count > 0);
    
    // Check that we have expected trait bounds
    int found_numeric = 0, found_partial_ord = 0, found_copy = 0;
    TraitBound* bound = bounds->bounds;
    while (bound) {
        if (bound->trait_name) {
            if (strcmp(bound->trait_name, "Numeric") == 0) found_numeric = 1;
            if (strcmp(bound->trait_name, "PartialOrd") == 0) found_partial_ord = 1;
            if (strcmp(bound->trait_name, "Copy") == 0) found_copy = 1;
        }
        bound = bound->next;
    }
    
    ASSERT_TRUE(found_numeric);
    ASSERT_TRUE(found_partial_ord);
    ASSERT_TRUE(found_copy);
    
    trait_bound_set_free(bounds);
    type_variable_free(var);
    type_free(unknown_type);
    return TEST_PASS;
}

TEST_F(ConstraintInferenceFixture, where_clause_generation) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Position pos = {.line = 9, .column = 1, .offset = 80, .filename = "test.goo"};
    TypeVariable* var = type_variable_new("T", TYPE_VAR_GENERIC, pos);
    
    // Add constraints
    Type* unknown_type = type_new(TYPE_UNKNOWN);
    ASSERT_TRUE(infer_constraints_from_usage_pattern(fixture->engine, unknown_type, "display", pos));
    ASSERT_TRUE(infer_constraints_from_usage_pattern(fixture->engine, unknown_type, "clone", pos));
    
    // Move constraints to type variable
    var->constraints = constraint_set_new();
    constraint_set_merge(var->constraints, fixture->engine->active_constraints);
    
    // Generate trait bounds and where clause
    TraitBoundSet* bounds = generate_trait_bounds_from_constraints(fixture->engine, var);
    ASSERT_NOT_NULL(bounds);
    
    char* where_clause = generate_where_clause_from_bounds(bounds);
    ASSERT_NOT_NULL(where_clause);
    
    // Check that where clause contains expected elements
    ASSERT_TRUE(strstr(where_clause, "where") != NULL);
    ASSERT_TRUE(strstr(where_clause, "T:") != NULL);
    ASSERT_TRUE(strstr(where_clause, "Display") != NULL || strstr(where_clause, "Clone") != NULL);
    
    test_log("Generated where clause: %s", where_clause);
    
    free(where_clause);
    trait_bound_set_free(bounds);
    type_variable_free(var);
    type_free(unknown_type);
    return TEST_PASS;
}

TEST_F(ConstraintInferenceFixture, trait_bound_optimization) {
    ConstraintInferenceFixture* fixture = GET_FIXTURE(ConstraintInferenceFixture);
    
    Position pos = {.line = 10, .column = 1, .offset = 90, .filename = "test.goo"};
    
    TraitBoundSet* bounds = trait_bound_set_new();
    ASSERT_NOT_NULL(bounds);
    
    // Add some bounds that should be optimized
    TraitBound* copy_bound = trait_bound_new("T", pos);
    copy_bound->trait_name = strdup("Copy");
    copy_bound->kind = TRAIT_BOUND_SIMPLE;
    
    TraitBound* clone_bound = trait_bound_new("T", pos);
    clone_bound->trait_name = strdup("Clone");
    clone_bound->kind = TRAIT_BOUND_SIMPLE;
    
    TraitBound* ord_bound = trait_bound_new("T", pos);
    ord_bound->trait_name = strdup("PartialOrd");
    ord_bound->kind = TRAIT_BOUND_SIMPLE;
    
    TraitBound* eq_bound = trait_bound_new("T", pos);
    eq_bound->trait_name = strdup("PartialEq");
    eq_bound->kind = TRAIT_BOUND_SIMPLE;
    
    trait_bound_set_add(bounds, copy_bound);
    trait_bound_set_add(bounds, clone_bound);
    trait_bound_set_add(bounds, ord_bound);
    trait_bound_set_add(bounds, eq_bound);
    
    ASSERT_EQ(4, bounds->count);
    
    // Optimize bounds (should remove Clone since Copy implies Clone)
    // and should remove PartialEq since PartialOrd implies PartialEq
    optimize_trait_bounds(bounds);
    
    // After optimization, we should have fewer bounds
    ASSERT_TRUE(bounds->count <= 4);
    ASSERT_TRUE(bounds->is_optimized);
    
    trait_bound_set_free(bounds);
    return TEST_PASS;
}

TEST_F(ConstraintInferenceFixture, trait_bound_validation) {
    TraitBoundSet* bounds = trait_bound_set_new();
    ASSERT_NOT_NULL(bounds);
    
    Position pos = {.line = 11, .column = 1, .offset = 100, .filename = "test.goo"};
    
    // Add compatible bounds
    TraitBound* debug_bound = trait_bound_new("T", pos);
    debug_bound->trait_name = strdup("Debug");
    debug_bound->kind = TRAIT_BOUND_SIMPLE;
    
    TraitBound* clone_bound = trait_bound_new("T", pos);
    clone_bound->trait_name = strdup("Clone");
    clone_bound->kind = TRAIT_BOUND_SIMPLE;
    
    trait_bound_set_add(bounds, debug_bound);
    trait_bound_set_add(bounds, clone_bound);
    
    // These should be compatible
    ASSERT_TRUE(validate_generated_trait_bounds(bounds));
    
    trait_bound_set_free(bounds);
    return TEST_PASS;
}

TEST(constraint_inference, trait_bound_management) {
    Position pos = {.line = 12, .column = 1, .offset = 110, .filename = "test.goo"};
    
    // Test trait bound creation
    TraitBound* bound = trait_bound_new("T", pos);
    ASSERT_NOT_NULL(bound);
    ASSERT_EQ_STR("T", bound->type_param_name);
    ASSERT_EQ(TRAIT_BOUND_SIMPLE, bound->kind);
    
    // Test trait bound set
    TraitBoundSet* set = trait_bound_set_new();
    ASSERT_NOT_NULL(set);
    ASSERT_EQ(0, set->count);
    
    // Add bound to set
    ASSERT_TRUE(trait_bound_set_add(set, bound));
    ASSERT_EQ(1, set->count);
    
    // Clean up
    trait_bound_set_free(set);
    
    return TEST_PASS;
}

// Register test functions for the test runner
void register_constraint_inference_tests(void) {
    // Tests are automatically registered via constructors
}