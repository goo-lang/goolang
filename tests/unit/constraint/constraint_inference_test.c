
#include "test/test_framework.h"
#include "types/constraint_inference.h"
#include "common/list.h"
#include "interface_system.h"
#include "errors/error.h"
#include <string.h>

// Test fixture for constraint inference
typedef struct {
    ErrorContext* error_ctx;
    TypeChecker* type_checker;
    ConstraintInferenceEngine* engine;
} ConstraintInferenceFixture;

// Setup function for the fixture
void setup_constraint_inference_fixture(void* fixture_data) {
    ConstraintInferenceFixture* fixture = (ConstraintInferenceFixture*)fixture_data;
    fixture->error_ctx = error_context_new();
    fixture->type_checker = type_checker_new();
    fixture->engine = constraint_inference_engine_new(fixture->error_ctx);
}

// Teardown function for the fixture
void teardown_constraint_inference_fixture(void* fixture_data) {
    ConstraintInferenceFixture* fixture = (ConstraintInferenceFixture*)fixture_data;
    constraint_inference_engine_free(fixture->engine);
    type_checker_free(fixture->type_checker);
    error_context_free(fixture->error_ctx);
}

// Basic constraint inference tests
TestStatus test_engine_creation(void* fixture_data) {
    ConstraintInferenceFixture* fixture = (ConstraintInferenceFixture*)fixture_data;
    
    ASSERT_NOT_NULL(fixture->engine);
    ASSERT_NOT_NULL(fixture->engine->unification_ctx);
    ASSERT_EQ_INT(0, fixture->engine->next_type_var_id);
    ASSERT_NULL(fixture->engine->type_var_pool);
    ASSERT_EQ_INT(0, fixture->engine->stats.constraints_generated);
    ASSERT_EQ_INT(0, fixture->engine->stats.constraints_resolved);
    ASSERT_EQ_INT(0, fixture->engine->stats.inference_failures);
    
    return TEST_PASS;
}

TestStatus test_add_simple_constraint(void* fixture_data) {
    ConstraintInferenceFixture* fixture = (ConstraintInferenceFixture*)fixture_data;

    Type* type_a = type_new_placeholder();
    Type* type_b = type_new_placeholder();

    Constraint* constraint = constraint_equality(type_a, type_b, CONSTRAINT_PRIORITY_MEDIUM, (Position){0});
    constraint_set_add(fixture->engine->unification_ctx->global_constraints, constraint);

    ASSERT_EQ_INT(1, fixture->engine->unification_ctx->global_constraints->count);
    ASSERT_EQ_PTR(constraint, fixture->engine->unification_ctx->global_constraints->constraints);

    type_free(type_a);
    type_free(type_b);
    constraint_free(constraint);

    return TEST_PASS;
}

// Register test functions for the test runner
void register_constraint_inference_tests(void) {
    register_test_with_fixture("constraint_inference", "engine_creation", &test_engine_creation, &setup_constraint_inference_fixture, &teardown_constraint_inference_fixture, __FILE__, __LINE__);
    register_test_with_fixture("constraint_inference", "add_simple_constraint", &test_add_simple_constraint, &setup_constraint_inference_fixture, &teardown_constraint_inference_fixture, __FILE__, __LINE__);
}
