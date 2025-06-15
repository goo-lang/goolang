#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "include/capability_security.h"
#include "include/security_framework.h"

void test_capability_token_creation() {
    printf("Testing capability token creation...\n");
    
    CapabilityToken* token = capability_token_create(CAP_READ_FILE, "test_function");
    assert(token != NULL);
    assert(token->capability == CAP_READ_FILE);
    assert(strcmp(token->granted_to, "test_function") == 0);
    assert(token->category == CAP_CATEGORY_FILE_SYSTEM);
    assert(!token->is_revoked);
    assert(token->can_delegate == false);
    assert(token->current_uses == 0);
    
    capability_token_destroy(token);
    printf("✓ Capability token creation test passed\n");
}

void test_capability_categories() {
    printf("Testing capability categorization...\n");
    
    // Test file system category
    CapabilityToken* fs_token = capability_token_create(CAP_WRITE_FILE, "fs_test");
    assert(fs_token->category == CAP_CATEGORY_FILE_SYSTEM);
    capability_token_destroy(fs_token);
    
    // Test network category
    CapabilityToken* net_token = capability_token_create(CAP_NETWORK_CONNECT, "net_test");
    assert(net_token->category == CAP_CATEGORY_NETWORK);
    capability_token_destroy(net_token);
    
    // Test crypto category
    CapabilityToken* crypto_token = capability_token_create(CAP_CRYPTO_ENCRYPT, "crypto_test");
    assert(crypto_token->category == CAP_CATEGORY_CRYPTO);
    capability_token_destroy(crypto_token);
    
    // Test process category
    CapabilityToken* proc_token = capability_token_create(CAP_PROCESS_SPAWN, "proc_test");
    assert(proc_token->category == CAP_CATEGORY_PROCESS);
    capability_token_destroy(proc_token);
    
    printf("✓ Capability categorization test passed\n");
}

void test_capability_policy_creation() {
    printf("Testing capability policy creation...\n");
    
    // Test custom policy
    CapabilityPolicy* custom_policy = capability_policy_create("custom");
    assert(custom_policy != NULL);
    assert(strcmp(custom_policy->policy_name, "custom") == 0);
    assert(custom_policy->allow_delegation == true);
    assert(custom_policy->max_delegation_depth == 3);
    capability_policy_destroy(custom_policy);
    
    // Test strict policy
    CapabilityPolicy* strict_policy = capability_policy_create_strict();
    assert(strict_policy != NULL);
    assert(strcmp(strict_policy->policy_name, "strict") == 0);
    assert(strict_policy->allow_delegation == false);
    assert(strict_policy->forbidden_capabilities & CAP_PRIVILEGE_ESCALATE);
    assert(strict_policy->default_function_capabilities == CAP_MEMORY_ALLOC);
    capability_policy_destroy(strict_policy);
    
    // Test moderate policy
    CapabilityPolicy* moderate_policy = capability_policy_create_moderate();
    assert(moderate_policy != NULL);
    assert(moderate_policy->allow_delegation == true);
    assert(moderate_policy->max_delegation_depth == 2);
    capability_policy_destroy(moderate_policy);
    
    // Test permissive policy
    CapabilityPolicy* permissive_policy = capability_policy_create_permissive();
    assert(permissive_policy != NULL);
    assert(permissive_policy->allow_delegation == true);
    assert(permissive_policy->max_delegation_depth == 5);
    assert(permissive_policy->forbidden_capabilities == 0);
    capability_policy_destroy(permissive_policy);
    
    printf("✓ Capability policy creation test passed\n");
}

void test_capability_context() {
    printf("Testing capability context...\n");
    
    CapabilityContext* global_ctx = capability_context_create("global", CONTEXT_GLOBAL);
    assert(global_ctx != NULL);
    assert(strcmp(global_ctx->context_name, "global") == 0);
    assert(global_ctx->context_type == CONTEXT_GLOBAL);
    assert(global_ctx->token_count == 0);
    
    CapabilityContext* func_ctx = capability_context_create("test_function", CONTEXT_FUNCTION);
    assert(func_ctx != NULL);
    assert(func_ctx->context_type == CONTEXT_FUNCTION);
    
    capability_context_destroy(func_ctx);
    capability_context_destroy(global_ctx);
    printf("✓ Capability context test passed\n");
}

void test_capability_system_creation() {
    printf("Testing capability system creation...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    assert(sec_ctx != NULL);
    
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    assert(cap_system != NULL);
    assert(cap_system->security_context == sec_ctx);
    assert(cap_system->global_context != NULL);
    assert(cap_system->checker != NULL);
    assert(cap_system->audit_enabled == true);
    assert(cap_system->strict_enforcement == true);
    
    // Initialize the system
    Result_void_ptr result = capability_system_initialize(cap_system);
    assert(!result.is_error);
    assert(cap_system->is_initialized);
    assert(cap_system->active_policy != NULL);
    assert(cap_system->policy_count > 0);
    
    capability_system_destroy(cap_system);
    security_context_destroy(sec_ctx);
    printf("✓ Capability system creation test passed\n");
}

void test_capability_grant_and_check() {
    printf("Testing capability grant and check...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    capability_system_initialize(cap_system);
    
    // Grant a capability
    Result_void_ptr grant_result = capability_system_grant(cap_system, CAP_READ_FILE, 
                                                   "test_function", 60000); // 1 minute
    assert(!grant_result.is_error);
    assert(cap_system->stats.total_grants == 1);
    assert(cap_system->stats.active_tokens == 1);
    
    // Check the capability - should succeed
    Result_bool check_result = capability_system_check(cap_system, "test_function", CAP_READ_FILE);
    assert(!check_result.is_error);
    assert(check_result.value == true);
    
    // Check a different capability - should fail
    check_result = capability_system_check(cap_system, "test_function", CAP_WRITE_FILE);
    assert(!check_result.is_error);
    assert(check_result.value == false);
    
    // Check a different entity - should fail
    check_result = capability_system_check(cap_system, "other_function", CAP_READ_FILE);
    assert(!check_result.is_error);
    assert(check_result.value == false);
    
    capability_system_destroy(cap_system);
    security_context_destroy(sec_ctx);
    printf("✓ Capability grant and check test passed\n");
}

void test_multiple_capabilities() {
    printf("Testing multiple capabilities...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    capability_system_initialize(cap_system);
    
    // Grant multiple capabilities to same entity
    capability_system_grant(cap_system, CAP_READ_FILE, "multi_test", 0);
    capability_system_grant(cap_system, CAP_WRITE_FILE, "multi_test", 0);
    capability_system_grant(cap_system, CAP_NETWORK_CONNECT, "multi_test", 0);
    
    assert(cap_system->stats.total_grants == 3);
    assert(cap_system->stats.active_tokens == 3);
    
    // Check all capabilities
    Result_bool result = capability_system_check(cap_system, "multi_test", CAP_READ_FILE);
    assert(!result.is_error && result.value);
    
    result = capability_system_check(cap_system, "multi_test", CAP_WRITE_FILE);
    assert(!result.is_error && result.value);
    
    result = capability_system_check(cap_system, "multi_test", CAP_NETWORK_CONNECT);
    assert(!result.is_error && result.value);
    
    // Check combined capabilities
    uint32_t combined = CAP_READ_FILE | CAP_WRITE_FILE;
    result = capability_system_check(cap_system, "multi_test", combined);
    assert(!result.is_error && result.value);
    
    capability_system_destroy(cap_system);
    security_context_destroy(sec_ctx);
    printf("✓ Multiple capabilities test passed\n");
}

void test_capability_expiration() {
    printf("Testing capability expiration...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    capability_system_initialize(cap_system);
    
    // Grant a capability with very short validity (1ms)
    Result_void_ptr grant_result = capability_system_grant(cap_system, CAP_READ_FILE, 
                                                   "expire_test", 1);
    assert(!grant_result.is_error);
    
    // Immediate check should succeed
    Result_bool check_result = capability_system_check(cap_system, "expire_test", CAP_READ_FILE);
    assert(!check_result.is_error);
    assert(check_result.value == true);
    
    // Wait for expiration
    usleep(2000); // 2ms
    
    // Check should now fail
    check_result = capability_system_check(cap_system, "expire_test", CAP_READ_FILE);
    assert(!check_result.is_error);
    assert(check_result.value == false);
    
    capability_system_destroy(cap_system);
    security_context_destroy(sec_ctx);
    printf("✓ Capability expiration test passed\n");
}

void test_capability_audit() {
    printf("Testing capability audit...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    capability_system_initialize(cap_system);
    
    assert(cap_system->audit_enabled);
    assert(cap_system->audit_count == 0);
    
    // Perform operations that should be audited
    capability_system_grant(cap_system, CAP_READ_FILE, "audit_test", 0);
    capability_system_check(cap_system, "audit_test", CAP_READ_FILE);
    capability_system_check(cap_system, "audit_test", CAP_WRITE_FILE); // Should fail
    capability_system_check(cap_system, "other_entity", CAP_READ_FILE); // Should fail
    
    // Check audit log
    assert(cap_system->audit_count >= 4);
    
    // Verify audit entries
    bool found_grant = false, found_use = false, found_deny = false;
    for (size_t i = 0; i < cap_system->audit_count; i++) {
        if (cap_system->audit_log[i].event_type == AUDIT_GRANT) {
            found_grant = true;
            assert(cap_system->audit_log[i].success);
        } else if (cap_system->audit_log[i].event_type == AUDIT_USE) {
            found_use = true;
            assert(cap_system->audit_log[i].success);
        } else if (cap_system->audit_log[i].event_type == AUDIT_DENY) {
            found_deny = true;
            assert(!cap_system->audit_log[i].success);
        }
    }
    
    assert(found_grant && found_use && found_deny);
    assert(cap_system->stats.total_denials >= 2);
    
    capability_system_destroy(cap_system);
    security_context_destroy(sec_ctx);
    printf("✓ Capability audit test passed\n");
}

void test_policy_enforcement() {
    printf("Testing policy enforcement...\n");
    
    // Test with strict policy
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_STRICT);
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    capability_system_initialize(cap_system);
    
    // Try to grant a forbidden capability
    Result_void_ptr result = capability_system_grant(cap_system, CAP_PRIVILEGE_ESCALATE, 
                                             "policy_test", 0);
    assert(result.is_error);
    assert(result.error->code == ERROR_CAPABILITY_SYS_DENIED);
    free(result.error);
    
    // Grant an allowed capability
    result = capability_system_grant(cap_system, CAP_MEMORY_ALLOC, "policy_test", 0);
    assert(!result.is_error);
    
    capability_system_destroy(cap_system);
    security_context_destroy(sec_ctx);
    printf("✓ Policy enforcement test passed\n");
}

void test_capability_checker() {
    printf("Testing capability checker...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    capability_system_initialize(cap_system);
    
    CapabilityChecker* checker = cap_system->checker;
    assert(checker != NULL);
    assert(checker->capability_system == cap_system);
    assert(checker->config.enable_inference);
    assert(checker->config.enable_static_checking);
    assert(checker->config.enable_runtime_checking);
    
    printf("✓ Capability checker test passed\n");
    
    capability_system_destroy(cap_system);
    security_context_destroy(sec_ctx);
}

void test_capability_requirement() {
    printf("Testing capability requirements...\n");
    
    CapabilityRequirement* req = capability_requirement_create(CAP_READ_FILE, 
                                                              "File read access required");
    assert(req != NULL);
    assert(req->required_capability == CAP_READ_FILE);
    assert(strcmp(req->description, "File read access required") == 0);
    assert(req->is_mandatory);
    assert(req->is_runtime_checked);
    
    capability_requirement_destroy(req);
    printf("✓ Capability requirement test passed\n");
}

void test_capability_integration() {
    printf("Testing capability system integration...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    capability_system_initialize(cap_system);
    
    // Test integration with security framework
    assert(cap_system->security_context == sec_ctx);
    assert(cap_system->active_policy != NULL);
    
    // Grant capabilities and verify integration
    capability_system_grant(cap_system, CAPABILITIES_FILESYSTEM, "integrated_test", 0);
    
    // Check individual capabilities from the set
    Result_bool result = capability_system_check(cap_system, "integrated_test", CAP_READ_FILE);
    assert(!result.is_error && result.value);
    
    result = capability_system_check(cap_system, "integrated_test", CAP_WRITE_FILE);
    assert(!result.is_error && result.value);
    
    result = capability_system_check(cap_system, "integrated_test", CAP_EXECUTE_FILE);
    assert(!result.is_error && result.value);
    
    // Verify statistics
    printf("  - Total grants: %llu\n", cap_system->stats.total_grants);
    printf("  - Active tokens: %llu\n", cap_system->stats.active_tokens);
    printf("  - Total denials: %llu\n", cap_system->stats.total_denials);
    printf("  - Audit entries: %zu\n", cap_system->audit_count);
    
    capability_system_destroy(cap_system);
    security_context_destroy(sec_ctx);
    printf("✓ Capability integration test passed\n");
}

int main() {
    printf("=== Capability-Based Security System Tests ===\n\n");
    
    test_capability_token_creation();
    test_capability_categories();
    test_capability_policy_creation();
    test_capability_context();
    test_capability_system_creation();
    test_capability_grant_and_check();
    test_multiple_capabilities();
    test_capability_expiration();
    test_capability_audit();
    test_policy_enforcement();
    test_capability_checker();
    test_capability_requirement();
    test_capability_integration();
    
    printf("\n=== All Capability Security Tests Passed! ===\n");
    return 0;
}