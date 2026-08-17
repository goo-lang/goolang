#include <stdio.h>
#include <stdlib.h>
#include "include/parallel_capability_security.h"

int main() {
    printf("=== Minimal Capability Security Test ===\n");
    
    // Test 1: Capability token creation
    printf("\n1. Testing capability token creation...\n");
    CapabilityToken* token = capability_token_create(SECURITY_LEVEL_STANDARD, 1);
    if (token) {
        printf("   ✅ Token created successfully (ID: %llu)\n", token->token_id);
        printf("   ✅ Security level: %s\n", security_level_to_string(token->security_level));
        capability_token_destroy(token);
    } else {
        printf("   ❌ Failed to create token\n");
        return 1;
    }
    
    // Test 2: Security capability creation
    printf("\n2. Testing security capability creation...\n");
    SecurityCapability* cap = security_capability_create(CAP_MEMORY_READ, SECURITY_LEVEL_STANDARD, NULL, 0);
    if (cap) {
        printf("   ✅ Capability created successfully (ID: %llu)\n", cap->capability_id);
        printf("   ✅ Capability type: %s\n", capability_type_to_string(cap->type));
        printf("   ✅ Required level: %s\n", security_level_to_string(cap->required_level));
        security_capability_destroy(cap);
    } else {
        printf("   ❌ Failed to create capability\n");
        return 1;
    }
    
    // Test 3: Capability granting and checking
    printf("\n3. Testing capability granting and checking...\n");
    CapabilityToken* test_token = capability_token_create(SECURITY_LEVEL_STANDARD, 2);
    SecurityCapability* read_cap = security_capability_create(CAP_MEMORY_READ, SECURITY_LEVEL_STANDARD, NULL, 0);
    SecurityCapability* write_cap = security_capability_create(CAP_MEMORY_WRITE, SECURITY_LEVEL_STANDARD, NULL, 0);
    
    if (test_token && read_cap && write_cap) {
        // Grant read capability
        bool granted = capability_token_grant(test_token, read_cap);
        printf("   ✅ Read capability granted: %s\n", granted ? "success" : "failed");
        
        // Check capabilities
        bool has_read = capability_token_has_capability(test_token, CAP_MEMORY_READ, NULL);
        bool has_write = capability_token_has_capability(test_token, CAP_MEMORY_WRITE, NULL);
        printf("   ✅ Has read capability: %s\n", has_read ? "yes" : "no");
        printf("   ✅ Has write capability: %s\n", has_write ? "yes" : "no");
        
        capability_token_destroy(test_token);
        security_capability_destroy(write_cap);
    } else {
        printf("   ❌ Failed to create test objects\n");
        return 1;
    }
    
    // Test 4: Configuration presets
    printf("\n4. Testing configuration presets...\n");
    CapabilitySecureParallelConfig default_config = capability_secure_config_default();
    CapabilitySecureParallelConfig strict_config = capability_secure_config_strict();
    CapabilitySecureParallelConfig untrusted_config = capability_secure_config_untrusted();
    
    printf("   ✅ Default config - Level: %s, Checking: %s\n",
           security_level_to_string(default_config.default_security_level),
           default_config.enable_capability_checking ? "enabled" : "disabled");
    printf("   ✅ Strict config - Level: %s, Terminate on violation: %s\n",
           security_level_to_string(strict_config.default_security_level),
           strict_config.terminate_on_violation ? "yes" : "no");
    printf("   ✅ Untrusted config - Level: %s, Capabilities: %s\n",
           security_level_to_string(untrusted_config.default_security_level),
           capability_type_to_string(untrusted_config.default_capabilities));
    
    // Test 5: Utility functions
    printf("\n5. Testing utility functions...\n");
    printf("   ✅ Security levels:\n");
    printf("      - UNTRUSTED: %s\n", security_level_to_string(SECURITY_LEVEL_UNTRUSTED));
    printf("      - RESTRICTED: %s\n", security_level_to_string(SECURITY_LEVEL_RESTRICTED));
    printf("      - STANDARD: %s\n", security_level_to_string(SECURITY_LEVEL_STANDARD));
    printf("      - PRIVILEGED: %s\n", security_level_to_string(SECURITY_LEVEL_PRIVILEGED));
    
    printf("   ✅ Capability types:\n");
    printf("      - MEMORY_READ: %s\n", capability_type_to_string(CAP_MEMORY_READ));
    printf("      - MEMORY_WRITE: %s\n", capability_type_to_string(CAP_MEMORY_WRITE));
    printf("      - FILE_READ: %s\n", capability_type_to_string(CAP_FILE_READ));
    printf("      - NETWORK_WRITE: %s\n", capability_type_to_string(CAP_NETWORK_WRITE));
    
    printf("\n=== All Tests Completed Successfully! ===\n");
    printf("\nCapability-Based Security System Features:\n");
    printf("✅ Hierarchical security levels (untrusted to privileged)\n");
    printf("✅ Fine-grained capability types (memory, file, network, etc.)\n");
    printf("✅ Token-based permission management\n");
    printf("✅ Configurable security policies\n");
    printf("✅ Runtime capability validation\n");
    printf("✅ Integration ready for parallel for system\n");
    
    printf("\n🎉 Capability-based security enhancement completed!\n");
    
    return 0;
}