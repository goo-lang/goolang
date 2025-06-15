#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "include/crypto_security.h"

void test_secure_memory_operations() {
    printf("Testing secure memory operations...\n");
    
    // Test secure malloc and free
    void* secure_mem = crypto_secure_malloc(1024);
    assert(secure_mem != NULL);
    
    // Write some data
    memset(secure_mem, 0xAA, 1024);
    
    // Test secure zero
    crypto_secure_memzero(secure_mem, 1024);
    
    // Verify memory is zeroed
    uint8_t* bytes = (uint8_t*)secure_mem;
    for (int i = 0; i < 1024; i++) {
        assert(bytes[i] == 0);
    }
    
    crypto_secure_free(secure_mem, 1024);
    printf("✓ Secure memory operations test passed\n");
}

void test_random_number_generation() {
    printf("Testing random number generation...\n");
    
    // Test random bytes
    uint8_t random_buffer[32];
    Result_void_ptr result = crypto_random_bytes(random_buffer, sizeof(random_buffer));
    assert(!result.is_error);
    
    // Check that we got some randomness (very basic test)
    bool all_zeros = true;
    for (size_t i = 0; i < sizeof(random_buffer); i++) {
        if (random_buffer[i] != 0) {
            all_zeros = false;
            break;
        }
    }
    assert(!all_zeros);
    
    // Test random integers
    uint32_t rand32_1 = crypto_random_uint32();
    uint32_t rand32_2 = crypto_random_uint32();
    assert(rand32_1 != rand32_2); // Very unlikely to be equal
    
    uint64_t rand64_1 = crypto_random_uint64();
    uint64_t rand64_2 = crypto_random_uint64();
    assert(rand64_1 != rand64_2); // Very unlikely to be equal
    
    printf("✓ Random number generation test passed\n");
}

void test_algorithm_properties() {
    printf("Testing algorithm properties...\n");
    
    // Test algorithm names
    assert(strcmp(crypto_algorithm_name(CRYPTO_AES_256_GCM), "AES-256-GCM") == 0);
    assert(strcmp(crypto_algorithm_name(CRYPTO_ED25519_SIGNATURE), "Ed25519-Signature") == 0);
    assert(strcmp(crypto_algorithm_name(CRYPTO_SHA3_256), "SHA3-256") == 0);
    
    // Test security levels
    assert(crypto_algorithm_security_level(CRYPTO_AES_256_GCM) == CRYPTO_SECURITY_LEVEL_256);
    assert(crypto_algorithm_security_level(CRYPTO_ED25519_SIGNATURE) == CRYPTO_SECURITY_LEVEL_128);
    assert(crypto_algorithm_security_level(CRYPTO_RSA_4096_OAEP) == CRYPTO_SECURITY_LEVEL_256);
    
    // Test post-quantum safety
    assert(crypto_algorithm_is_post_quantum_safe(CRYPTO_AES_256_GCM) == true);
    assert(crypto_algorithm_is_post_quantum_safe(CRYPTO_SHA3_256) == true);
    assert(crypto_algorithm_is_post_quantum_safe(CRYPTO_RSA_4096_OAEP) == false);
    assert(crypto_algorithm_is_post_quantum_safe(CRYPTO_ED25519_SIGNATURE) == false);
    
    // Test deprecated algorithms
    assert(crypto_algorithm_is_deprecated(CRYPTO_AES_256_CBC) == true);
    assert(crypto_algorithm_is_deprecated(CRYPTO_AES_256_GCM) == false);
    assert(crypto_algorithm_is_deprecated(CRYPTO_ED25519_SIGNATURE) == false);
    
    printf("✓ Algorithm properties test passed\n");
}

void test_crypto_context_operations() {
    printf("Testing crypto context operations...\n");
    
    // Create context
    CryptoContext* context = crypto_context_create(CRYPTO_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256);
    assert(context != NULL);
    assert(context->algorithm == CRYPTO_AES_256_GCM);
    assert(context->min_security_level == CRYPTO_SECURITY_LEVEL_256);
    assert(context->auto_generate_nonce == true);
    assert(context->nonce != NULL);
    assert(context->nonce_length == 12); // AES-GCM uses 96-bit nonce
    
    // Create security context and associate it
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_STRICT);
    Result_void_ptr result = crypto_context_set_security_context(context, sec_ctx);
    assert(!result.is_error);
    assert(context->security_context == sec_ctx);
    
    // Test ChaCha20 context
    CryptoContext* chacha_context = crypto_context_create(CRYPTO_CHACHA20_POLY1305, CRYPTO_SECURITY_LEVEL_256);
    assert(chacha_context != NULL);
    assert(chacha_context->nonce_length == 24); // ChaCha20 uses 192-bit nonce
    
    crypto_context_destroy(chacha_context);
    crypto_context_destroy(context);
    security_context_destroy(sec_ctx);
    
    printf("✓ Crypto context operations test passed\n");
}

void test_symmetric_key_generation() {
    printf("Testing symmetric key generation...\n");
    
    // Generate AES-256-GCM key
    CryptoKey* aes_key = crypto_key_generate(CRYPTO_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256, "test_encryption");
    assert(aes_key != NULL);
    assert(aes_key->key_type == CRYPTO_KEY_SYMMETRIC);
    assert(aes_key->algorithm == CRYPTO_AES_256_GCM);
    assert(aes_key->security_level == CRYPTO_SECURITY_LEVEL_256);
    assert(aes_key->key_length == 32); // 256 bits
    assert(aes_key->key_material != NULL);
    assert(aes_key->key_source == CRYPTO_KEY_SOURCE_GENERATED);
    assert(strcmp(aes_key->purpose, "test_encryption") == 0);
    assert(strstr(aes_key->allowed_operations, "encrypt") != NULL);
    assert(strstr(aes_key->allowed_operations, "decrypt") != NULL);
    
    // Test key validity
    Result_bool valid_result = crypto_key_is_valid(aes_key);
    assert(!valid_result.is_error);
    assert(valid_result.data == true);
    
    // Test operation permissions
    Result_bool can_encrypt = crypto_key_can_perform_operation(aes_key, "encrypt");
    assert(!can_encrypt.is_error);
    assert(can_encrypt.data == true);
    
    Result_bool can_sign = crypto_key_can_perform_operation(aes_key, "sign");
    assert(!can_sign.is_error);
    assert(can_sign.data == false);
    
    // Generate ChaCha20 key
    CryptoKey* chacha_key = crypto_key_generate(CRYPTO_CHACHA20_POLY1305, CRYPTO_SECURITY_LEVEL_256, "test_stream_cipher");
    assert(chacha_key != NULL);
    assert(chacha_key->key_type == CRYPTO_KEY_SYMMETRIC);
    assert(chacha_key->key_length == 32); // 256 bits
    
    crypto_key_destroy(chacha_key);
    crypto_key_destroy(aes_key);
    
    printf("✓ Symmetric key generation test passed\n");
}

void test_asymmetric_key_generation() {
    printf("Testing asymmetric key generation...\n");
    
    // Generate Ed25519 keypair
    CryptoKeyPair* ed25519_pair = crypto_keypair_generate(CRYPTO_ED25519_SIGNATURE, CRYPTO_SECURITY_LEVEL_128, "test_signing");
    assert(ed25519_pair != NULL);
    assert(ed25519_pair->private_key != NULL);
    assert(ed25519_pair->public_key != NULL);
    assert(ed25519_pair->private_key->key_type == CRYPTO_KEY_ASYMMETRIC_PRIVATE);
    assert(ed25519_pair->public_key->key_type == CRYPTO_KEY_ASYMMETRIC_PUBLIC);
    assert(ed25519_pair->private_key->key_length == 32); // 256 bits
    assert(ed25519_pair->public_key->key_length == 32); // 256 bits
    assert(ed25519_pair->private_key->is_exportable == false);
    assert(ed25519_pair->public_key->is_exportable == true);
    assert(strstr(ed25519_pair->private_key->allowed_operations, "sign") != NULL);
    assert(strstr(ed25519_pair->public_key->allowed_operations, "verify") != NULL);
    assert(strcmp(ed25519_pair->pair_name, "test_signing") == 0);
    
    // Generate Curve25519 keypair
    CryptoKeyPair* x25519_pair = crypto_keypair_generate(CRYPTO_CURVE25519_X25519, CRYPTO_SECURITY_LEVEL_128, "test_key_exchange");
    assert(x25519_pair != NULL);
    assert(x25519_pair->private_key->algorithm == CRYPTO_CURVE25519_X25519);
    assert(x25519_pair->public_key->algorithm == CRYPTO_CURVE25519_X25519);
    assert(strstr(x25519_pair->private_key->allowed_operations, "key_exchange") != NULL);
    assert(strstr(x25519_pair->public_key->allowed_operations, "key_exchange") != NULL);
    
    // Generate RSA-4096 keypair
    CryptoKeyPair* rsa_pair = crypto_keypair_generate(CRYPTO_RSA_4096_OAEP, CRYPTO_SECURITY_LEVEL_256, "test_rsa");
    assert(rsa_pair != NULL);
    assert(rsa_pair->private_key->key_length == 512); // 4096 bits
    assert(rsa_pair->public_key->key_length == 512); // 4096 bits
    assert(rsa_pair->private_key->security_level == CRYPTO_SECURITY_LEVEL_256);
    
    crypto_keypair_destroy(rsa_pair);
    crypto_keypair_destroy(x25519_pair);
    crypto_keypair_destroy(ed25519_pair);
    
    printf("✓ Asymmetric key generation test passed\n");
}

void test_key_access_control() {
    printf("Testing key access control...\n");
    
    CryptoKey* key = crypto_key_generate(CRYPTO_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256, "test_access_control");
    assert(key != NULL);
    
    // Set access control
    Result_void_ptr result = crypto_key_set_access_control(key, CAP_CRYPTO_SIGN | CAP_CRYPTO_ENCRYPT, "test_user");
    assert(!result.is_error);
    assert(key->required_capabilities == (CAP_CRYPTO_SIGN | CAP_CRYPTO_ENCRYPT));
    assert(strcmp(key->owner, "test_user") == 0);
    
    crypto_key_destroy(key);
    
    printf("✓ Key access control test passed\n");
}

void test_key_rotation() {
    printf("Testing key rotation...\n");
    
    CryptoKey* key = crypto_key_generate(CRYPTO_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256, "test_rotation");
    assert(key != NULL);
    
    uint64_t original_key_id = key->key_id;
    uint8_t original_material[32];
    memcpy(original_material, key->key_material, 32);
    
    // Rotate the key
    Result_void_ptr result = crypto_key_rotate(key);
    assert(!result.is_error);
    
    // Verify rotation occurred
    assert(key->key_id != original_key_id);
    assert(key->previous_key_id == original_key_id);
    assert(key->rotation_count == 1);
    assert(key->last_rotated > 0);
    assert(key->usage_count == 0); // Should be reset
    
    // Verify key material changed
    bool material_changed = (memcmp(key->key_material, original_material, 32) != 0);
    assert(material_changed);
    
    crypto_key_destroy(key);
    
    printf("✓ Key rotation test passed\n");
}

void test_crypto_policies() {
    printf("Testing crypto policies...\n");
    
    // Test default policy
    CryptoPolicy* default_policy = crypto_policy_create_default();
    assert(default_policy != NULL);
    assert(strcmp(default_policy->policy_name, "Default Security Policy") == 0);
    assert(default_policy->preferred_symmetric_encryption[0] == CRYPTO_AES_256_GCM);
    assert(default_policy->preferred_asymmetric_encryption[0] == CRYPTO_CURVE25519_X25519);
    assert(default_policy->preferred_signature_algorithms[0] == CRYPTO_ED25519_SIGNATURE);
    assert(default_policy->min_security_level == CRYPTO_SECURITY_LEVEL_128);
    assert(default_policy->automatic_key_rotation == true);
    assert(default_policy->audit_all_operations == true);
    
    // Test high security policy
    CryptoPolicy* high_sec_policy = crypto_policy_create_high_security();
    assert(high_sec_policy != NULL);
    assert(strcmp(high_sec_policy->policy_name, "High Security Policy") == 0);
    assert(high_sec_policy->min_security_level == CRYPTO_SECURITY_LEVEL_256);
    assert(high_sec_policy->require_hardware_keys == true);
    assert(high_sec_policy->require_forward_secrecy == true);
    assert(high_sec_policy->fips_140_2_required == true);
    assert(high_sec_policy->symmetric_key_lifetime_days == 30);
    
    // Test policy application
    Result_void_ptr apply_result = crypto_policy_apply(default_policy);
    assert(!apply_result.is_error);
    
    // Test policy validation
    Result_void_ptr validate_result = crypto_policy_validate_operation(default_policy, CRYPTO_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256);
    assert(!validate_result.is_error);
    
    // Test policy violation - insufficient security level
    Result_void_ptr violation_result = crypto_policy_validate_operation(high_sec_policy, CRYPTO_AES_256_GCM, CRYPTO_SECURITY_LEVEL_128);
    assert(violation_result.is_error);
    assert(violation_result.error->code == ERROR_CRYPTO_INSUFFICIENT_SECURITY);
    
    // Test deprecated algorithm violation
    Result_void_ptr deprecated_result = crypto_policy_validate_operation(default_policy, CRYPTO_AES_256_CBC, CRYPTO_SECURITY_LEVEL_256);
    assert(deprecated_result.is_error);
    assert(deprecated_result.error->code == ERROR_CRYPTO_POLICY_VIOLATION);
    
    crypto_policy_destroy(high_sec_policy);
    crypto_policy_destroy(default_policy);
    
    printf("✓ Crypto policies test passed\n");
}

void test_integration_with_security_framework() {
    printf("Testing integration with security framework...\n");
    
    // Create security context
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_STRICT);
    assert(sec_ctx != NULL);
    
    // Create crypto context with security integration
    CryptoContext* crypto_ctx = crypto_context_create(CRYPTO_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256);
    Result_void_ptr result = crypto_context_set_security_context(crypto_ctx, sec_ctx);
    assert(!result.is_error);
    
    // Generate key with access control
    CryptoKey* key = crypto_key_generate(CRYPTO_ED25519_SIGNATURE, CRYPTO_SECURITY_LEVEL_128, "integration_test");
    result = crypto_key_set_access_control(key, CAP_CRYPTO_SIGN, "test_entity");
    assert(!result.is_error);
    
    // Verify integration
    assert(crypto_ctx->security_context == sec_ctx);
    assert(crypto_ctx->audit_operations == true);
    assert(key->required_capabilities == CAP_CRYPTO_SIGN);
    
    crypto_key_destroy(key);
    crypto_context_destroy(crypto_ctx);
    security_context_destroy(sec_ctx);
    
    printf("✓ Integration with security framework test passed\n");
}

void test_error_handling() {
    printf("Testing error handling...\n");
    
    // Test invalid parameters
    CryptoKey* invalid_key = crypto_key_generate((CryptoAlgorithm)9999, CRYPTO_SECURITY_LEVEL_256, "invalid");
    assert(invalid_key == NULL);
    
    // Test invalid symmetric algorithm for keypair generation
    CryptoKeyPair* invalid_pair = crypto_keypair_generate(CRYPTO_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256, "invalid");
    assert(invalid_pair == NULL);
    
    // Test null key validation
    Result_bool null_valid = crypto_key_is_valid(NULL);
    assert(null_valid.is_error == true);
    
    // Test null operation check
    Result_bool null_operation = crypto_key_can_perform_operation(NULL, "encrypt");
    assert(null_operation.is_error == true);
    
    // Test invalid random bytes
    Result_void_ptr null_random = crypto_random_bytes(NULL, 32);
    assert(null_random.is_error == true);
    
    printf("✓ Error handling test passed\n");
}

void test_performance_and_scalability() {
    printf("Testing performance and scalability...\n");
    
    clock_t start_time = clock();
    
    // Generate multiple keys to test performance
    const int key_count = 100;
    CryptoKey* keys[key_count];
    
    for (int i = 0; i < key_count; i++) {
        keys[i] = crypto_key_generate(CRYPTO_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256, "performance_test");
        assert(keys[i] != NULL);
    }
    
    clock_t generation_time = clock();
    
    // Test key operations
    for (int i = 0; i < key_count; i++) {
        Result_bool valid = crypto_key_is_valid(keys[i]);
        assert(!valid.is_error && valid.data);
        
        Result_bool can_encrypt = crypto_key_can_perform_operation(keys[i], "encrypt");
        assert(!can_encrypt.is_error && can_encrypt.data);
    }
    
    clock_t operation_time = clock();
    
    // Cleanup
    for (int i = 0; i < key_count; i++) {
        crypto_key_destroy(keys[i]);
    }
    
    clock_t cleanup_time = clock();
    
    double generation_ms = ((double)(generation_time - start_time) / CLOCKS_PER_SEC) * 1000;
    double operation_ms = ((double)(operation_time - generation_time) / CLOCKS_PER_SEC) * 1000;
    double cleanup_ms = ((double)(cleanup_time - operation_time) / CLOCKS_PER_SEC) * 1000;
    
    printf("  - Generated %d keys in %.2f ms (%.2f ms per key)\n", key_count, generation_ms, generation_ms / key_count);
    printf("  - Performed %d key operations in %.2f ms\n", key_count * 2, operation_ms);
    printf("  - Cleaned up %d keys in %.2f ms\n", key_count, cleanup_ms);
    
    printf("✓ Performance and scalability test passed\n");
}

int main() {
    printf("=== Cryptographic Security System Tests ===\n\n");
    
    test_secure_memory_operations();
    test_random_number_generation();
    test_algorithm_properties();
    test_crypto_context_operations();
    test_symmetric_key_generation();
    test_asymmetric_key_generation();
    test_key_access_control();
    test_key_rotation();
    test_crypto_policies();
    test_integration_with_security_framework();
    test_error_handling();
    test_performance_and_scalability();
    
    printf("\n=== All Cryptographic Security Tests Passed! ===\n");
    return 0;
}