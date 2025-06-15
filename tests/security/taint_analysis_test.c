#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "include/taint_analysis.h"
#include "include/security_framework.h"

void test_taint_info_creation() {
    printf("Testing taint info creation...\n");
    
    TaintInfo* info = taint_info_create(TAINT_USER_INPUT, TAINT_SOURCE_USER_INPUT, "test_location");
    assert(info != NULL);
    assert(info->level == TAINT_USER_INPUT);
    assert(info->source_type == TAINT_SOURCE_USER_INPUT);
    assert(strcmp(info->source_location, "test_location") == 0);
    assert(info->confidence_score == 1.0);
    assert(!info->is_sanitized);
    assert(!info->is_validated);
    
    taint_info_destroy(info);
    printf("✓ Taint info creation test passed\n");
}

void test_taint_info_combination() {
    printf("Testing taint info combination...\n");
    
    TaintInfo* info1 = taint_info_create(TAINT_USER_INPUT, TAINT_SOURCE_USER_INPUT, "location1");
    TaintInfo* info2 = taint_info_create(TAINT_NETWORK, TAINT_SOURCE_NETWORK_SOCKET, "location2");
    
    TaintInfo* combined = taint_info_combine(info1, info2);
    assert(combined != NULL);
    assert(combined->level == TAINT_NETWORK); // Higher taint level
    assert(combined->parent_count == 2);
    assert(combined->parents[0] == info1);
    assert(combined->parents[1] == info2);
    
    taint_info_destroy(combined);
    // Note: Don't destroy info1 and info2 as they're referenced by combined
    printf("✓ Taint info combination test passed\n");
}

void test_taint_analyzer_creation() {
    printf("Testing taint analyzer creation...\n");
    
    SecurityContext* security_context = security_context_create(SECURITY_POLICY_STRICT);
    assert(security_context != NULL);
    
    TaintAnalyzer* analyzer = taint_analyzer_create(security_context);
    assert(analyzer != NULL);
    assert(analyzer->security_context == security_context);
    assert(analyzer->inference_engine != NULL);
    assert(analyzer->config.enable_automatic_inference == true);
    
    // Test initialization
    Result_void_ptr result = taint_analyzer_initialize(analyzer);
    assert(!result.is_error);
    
    taint_analyzer_destroy(analyzer);
    security_context_destroy(security_context);
    printf("✓ Taint analyzer creation test passed\n");
}

void test_taint_inference_engine() {
    printf("Testing taint inference engine...\n");
    
    TaintInferenceEngine* engine = taint_inference_engine_create();
    assert(engine != NULL);
    assert(engine->signature_capacity == 100);
    assert(engine->signature_count == 0);
    
    taint_inference_engine_destroy(engine);
    printf("✓ Taint inference engine test passed\n");
}

void test_builtin_source_registration() {
    printf("Testing builtin source registration...\n");
    
    SecurityContext* security_context = security_context_create(SECURITY_POLICY_STRICT);
    TaintAnalyzer* analyzer = taint_analyzer_create(security_context);
    
    Result_void_ptr result = taint_register_builtin_sources(analyzer);
    assert(!result.is_error);
    
    // Verify some sources were registered
    assert(analyzer->inference_engine->signature_count > 0);
    
    bool found_scanf = false;
    for (size_t i = 0; i < analyzer->inference_engine->signature_count; i++) {
        if (strcmp(analyzer->inference_engine->function_signatures[i].function_name, "scanf") == 0) {
            found_scanf = true;
            assert(analyzer->inference_engine->function_signatures[i].source_type == TAINT_SOURCE_USER_INPUT);
            break;
        }
    }
    assert(found_scanf);
    
    taint_analyzer_destroy(analyzer);
    security_context_destroy(security_context);
    printf("✓ Builtin source registration test passed\n");
}

void test_builtin_sink_registration() {
    printf("Testing builtin sink registration...\n");
    
    SecurityContext* security_context = security_context_create(SECURITY_POLICY_STRICT);
    TaintAnalyzer* analyzer = taint_analyzer_create(security_context);
    
    Result_void_ptr result = taint_register_builtin_sinks(analyzer);
    assert(!result.is_error);
    
    // Verify some sinks were registered
    bool found_system = false;
    for (size_t i = 0; i < analyzer->inference_engine->signature_count; i++) {
        if (strcmp(analyzer->inference_engine->function_signatures[i].function_name, "system") == 0) {
            found_system = true;
            assert(analyzer->inference_engine->function_signatures[i].sink_type == TAINT_SINK_COMMAND_EXECUTION);
            break;
        }
    }
    assert(found_system);
    
    taint_analyzer_destroy(analyzer);
    security_context_destroy(security_context);
    printf("✓ Builtin sink registration test passed\n");
}

void test_builtin_sanitizer_registration() {
    printf("Testing builtin sanitizer registration...\n");
    
    SecurityContext* security_context = security_context_create(SECURITY_POLICY_STRICT);
    TaintAnalyzer* analyzer = taint_analyzer_create(security_context);
    
    Result_void_ptr result = taint_register_builtin_sanitizers(analyzer);
    assert(!result.is_error);
    
    // Verify some sanitizers were registered
    bool found_html_escape = false;
    for (size_t i = 0; i < analyzer->inference_engine->signature_count; i++) {
        if (strcmp(analyzer->inference_engine->function_signatures[i].function_name, "html_escape") == 0) {
            found_html_escape = true;
            assert(analyzer->inference_engine->function_signatures[i].recommended_sanitization == SANITIZE_HTML_ESCAPE);
            break;
        }
    }
    assert(found_html_escape);
    
    taint_analyzer_destroy(analyzer);
    security_context_destroy(security_context);
    printf("✓ Builtin sanitizer registration test passed\n");
}

void test_propagation_rules() {
    printf("Testing propagation rules...\n");
    
    TaintPropagationRule* assignment_rule = taint_rule_create_assignment();
    assert(assignment_rule != NULL);
    assert(strcmp(assignment_rule->rule_name, "assignment") == 0);
    assert(assignment_rule->propagation_type == PROPAGATION_PRESERVE);
    assert(assignment_rule->is_enabled == true);
    free(assignment_rule);
    
    TaintPropagationRule* arithmetic_rule = taint_rule_create_arithmetic();
    assert(arithmetic_rule != NULL);
    assert(strcmp(arithmetic_rule->rule_name, "arithmetic") == 0);
    assert(arithmetic_rule->propagation_type == PROPAGATION_COMBINE);
    free(arithmetic_rule);
    
    TaintPropagationRule* string_rule = taint_rule_create_string_operation();
    assert(string_rule != NULL);
    assert(strcmp(string_rule->rule_name, "string_operation") == 0);
    assert(string_rule->propagation_type == PROPAGATION_COMBINE);
    free(string_rule);
    
    printf("✓ Propagation rules test passed\n");
}

void test_compiler_integration() {
    printf("Testing compiler integration...\n");
    
    SecurityContext* security_context = security_context_create(SECURITY_POLICY_STRICT);
    TaintAnalyzer* analyzer = taint_analyzer_create(security_context);
    
    CompilerTaintIntegration* integration = compiler_taint_integration_create(analyzer);
    assert(integration != NULL);
    assert(integration->analyzer == analyzer);
    assert(integration->integrate_during_type_checking == true);
    assert(integration->generate_runtime_taint_checks == true);
    
    compiler_taint_integration_destroy(integration);
    taint_analyzer_destroy(analyzer);
    security_context_destroy(security_context);
    printf("✓ Compiler integration test passed\n");
}

void test_configuration() {
    printf("Testing configuration...\n");
    
    TaintAnalysisConfig default_config = taint_analysis_config_default();
    assert(default_config.enable_automatic_inference == true);
    assert(default_config.minimum_confidence_threshold == 0.7);
    assert(default_config.max_analysis_depth == 50);
    
    TaintAnalysisConfig strict_config = taint_analysis_config_strict();
    assert(strict_config.minimum_confidence_threshold == 0.5);
    assert(strict_config.max_analysis_depth == 100);
    assert(strict_config.enable_statistical_inference == true);
    
    TaintAnalysisConfig permissive_config = taint_analysis_config_permissive();
    assert(permissive_config.minimum_confidence_threshold == 0.9);
    assert(permissive_config.generate_runtime_checks == false);
    
    printf("✓ Configuration test passed\n");
}

void test_taint_analysis_integration() {
    printf("Testing taint analysis integration with security framework...\n");
    
    SecurityContext* security_context = security_context_create(SECURITY_POLICY_STRICT);
    assert(security_context != NULL);
    
    TaintAnalyzer* analyzer = taint_analyzer_create(security_context);
    assert(analyzer != NULL);
    
    // Initialize the analyzer with full setup
    Result_void_ptr result = taint_analyzer_initialize(analyzer);
    assert(!result.is_error);
    
    // Verify that the analyzer is properly integrated with the security framework
    assert(analyzer->security_context == security_context);
    assert(analyzer->config.integrate_with_security_framework == true);
    
    // Test that the inference engine has been populated with builtin sources, sinks, and sanitizers
    assert(analyzer->inference_engine->signature_count > 0);
    
    printf("  - Registered %zu function signatures\n", analyzer->inference_engine->signature_count);
    printf("  - Security context integration: %s\n", 
           analyzer->config.integrate_with_security_framework ? "enabled" : "disabled");
    printf("  - Runtime checks: %s\n", 
           analyzer->config.generate_runtime_checks ? "enabled" : "disabled");
    
    taint_analyzer_destroy(analyzer);
    security_context_destroy(security_context);
    printf("✓ Taint analysis integration test passed\n");
}

int main() {
    printf("=== Taint Analysis System Tests ===\n\n");
    
    test_taint_info_creation();
    test_taint_info_combination();
    test_taint_analyzer_creation();
    test_taint_inference_engine();
    test_builtin_source_registration();
    test_builtin_sink_registration();
    test_builtin_sanitizer_registration();
    test_propagation_rules();
    test_compiler_integration();
    test_configuration();
    test_taint_analysis_integration();
    
    printf("\n=== All Taint Analysis Tests Passed! ===\n");
    return 0;
}