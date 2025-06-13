// Simple test for Enhanced Interface System compilation and basic functionality
#include "interface_system.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>

// Mock TypeChecker for testing
static TypeChecker* create_mock_type_checker() {
    TypeChecker* checker = malloc(sizeof(TypeChecker));
    if (!checker) return NULL;
    
    // Initialize all fields to NULL/0
    memset(checker, 0, sizeof(TypeChecker));
    
    return checker;
}

// Test basic initialization
int test_basic_initialization() {
    printf("Testing Enhanced Interface System initialization...\n");
    
    TypeChecker* checker = create_mock_type_checker();
    if (!checker) {
        printf("❌ Failed to create mock type checker\n");
        return 0;
    }
    
    // Test initialization
    int result = type_checker_init_enhanced_interfaces(checker);
    if (!result) {
        printf("❌ Failed to initialize enhanced interfaces\n");
        free(checker);
        return 0;
    }
    
    printf("✅ Enhanced interface system initialized successfully\n");
    
    // Test cleanup
    type_checker_cleanup_enhanced_interfaces(checker);
    printf("✅ Enhanced interface system cleaned up successfully\n");
    
    free(checker);
    return 1;
}

// Test constraint inference engine
int test_constraint_inference() {
    printf("Testing constraint inference engine...\n");
    
    TypeChecker* checker = create_mock_type_checker();
    if (!checker) return 0;
    
    ConstraintInferenceEngine* engine = constraint_inference_engine_new(checker);
    if (!engine) {
        printf("❌ Failed to create constraint inference engine\n");
        free(checker);
        return 0;
    }
    
    printf("✅ Constraint inference engine created successfully\n");
    
    constraint_inference_engine_free(engine);
    printf("✅ Constraint inference engine freed successfully\n");
    
    free(checker);
    return 1;
}

// Test registry creation
int test_registries() {
    printf("Testing registries...\n");
    
    // Test ConceptRegistry
    ConceptRegistry* concept_reg = concept_registry_new();
    if (!concept_reg) {
        printf("❌ Failed to create concept registry\n");
        return 0;
    }
    printf("✅ Concept registry created successfully\n");
    concept_registry_free(concept_reg);
    
    // Test HKTRegistry  
    HKTRegistry* hkt_reg = hkt_registry_new();
    if (!hkt_reg) {
        printf("❌ Failed to create HKT registry\n");
        return 0;
    }
    printf("✅ HKT registry created successfully\n");
    hkt_registry_free(hkt_reg);
    
    // Test ProtocolRegistry
    ProtocolRegistry* protocol_reg = protocol_registry_new();
    if (!protocol_reg) {
        printf("❌ Failed to create protocol registry\n");
        return 0;
    }
    printf("✅ Protocol registry created successfully\n");
    protocol_registry_free(protocol_reg);
    
    return 1;
}

int main() {
    printf("Enhanced Interface System - Basic Tests\n");
    printf("=====================================\n\n");
    
    int tests_passed = 0;
    int total_tests = 3;
    
    if (test_basic_initialization()) tests_passed++;
    if (test_constraint_inference()) tests_passed++;  
    if (test_registries()) tests_passed++;
    
    printf("\n=====================================\n");
    printf("Test Results: %d/%d tests passed\n", tests_passed, total_tests);
    
    if (tests_passed == total_tests) {
        printf("🎉 All tests passed! Enhanced Interface System is working.\n");
        return 0;
    } else {
        printf("❌ Some tests failed.\n");
        return 1;
    }
}
