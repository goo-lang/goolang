#include "test/test_framework.h"
#include "interface_system.h"
#include "errors/error.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test concept type creation
TEST(concept_declaration, concept_type_creation) {
    Type* concept_type = type_concept("TestConcept");
    
    ASSERT_NOT_NULL(concept_type);
    ASSERT_EQ(concept_type->kind, TYPE_CONCEPT);
    ASSERT_EQ_STR(concept_type->data.concept.name, "TestConcept");
    ASSERT_EQ_STR(concept_type->name, "TestConcept");
    ASSERT_EQ(concept_type->size, 0);  // Concepts are compile-time only
    ASSERT_EQ(concept_type->align, 0);
    
    type_free(concept_type);
    return TEST_PASS;
}

// Test concept type with null name
TEST(concept_declaration, concept_type_null_name) {
    Type* concept_type = type_concept(NULL);
    ASSERT_NULL(concept_type);
    return TEST_PASS;
}

// Test concept type freeing
TEST(concept_declaration, concept_type_freeing) {
    Type* concept_type = type_concept("FreeMeConcept");
    ASSERT_NOT_NULL(concept_type);
    
    // This should not crash
    type_free(concept_type);
    
    // Test freeing NULL concept
    type_free(NULL);
    return TEST_PASS;
}

// Test concept declaration basic structure  
TEST(concept_declaration, concept_declaration_structure) {
    ConceptDeclNode* concept = ast_concept_decl_new("TestConcept", (Position){1, 1, 0, "test.goo"});
    
    ASSERT_NOT_NULL(concept);
    ASSERT_EQ(concept->base.type, AST_CONCEPT_DECL);
    ASSERT_EQ_STR(concept->name, "TestConcept");
    ASSERT_NULL(concept->type_params);
    ASSERT_NULL(concept->requirements);
    
    ast_node_free((ASTNode*)concept);
    return TEST_PASS;
}
