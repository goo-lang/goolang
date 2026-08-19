#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "include/crypto_security.h"

void test_algorithm_names() {
    printf("Testing algorithm names...\n");
    
    const char* aes_name = crypto_algorithm_name(CRYPTO_EXTENDED_AES_256_GCM);
    assert(strcmp(aes_name, "AES-256-GCM") == 0);
    
    const char* ed25519_name = crypto_algorithm_name(CRYPTO_EXTENDED_ED25519_SIGNATURE);
    assert(strcmp(ed25519_name, "Ed25519-Signature") == 0);
    
    printf("✓ Algorithm names test passed\n");
}

void test_security_levels() {
    printf("Testing security levels...\n");
    
    assert(crypto_algorithm_security_level(CRYPTO_EXTENDED_AES_256_GCM) == CRYPTO_SECURITY_LEVEL_256);
    assert(crypto_algorithm_security_level(CRYPTO_EXTENDED_ED25519_SIGNATURE) == CRYPTO_SECURITY_LEVEL_128);
    assert(crypto_algorithm_security_level(CRYPTO_EXTENDED_RSA_4096_OAEP) == CRYPTO_SECURITY_LEVEL_256);
    
    printf("✓ Security levels test passed\n");
}

void test_random_generation() {
    printf("Testing random number generation...\n");
    
    uint8_t buffer[32];
    Result_void_ptr result = crypto_random_bytes(buffer, sizeof(buffer));
    assert(!result.is_error);
    
    uint32_t rand32 = crypto_random_uint32();
    uint64_t rand64 = crypto_random_uint64();
    
    // Basic sanity check - numbers should be different
    assert(rand32 != 0 || rand64 != 0);
    
    printf("✓ Random generation test passed\n");
}

void test_crypto_context() {
    printf("Testing crypto context...\n");
    
    CryptoContext* context = crypto_context_create(CRYPTO_EXTENDED_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256);
    assert(context != NULL);
    assert(context->algorithm == CRYPTO_EXTENDED_AES_256_GCM);
    assert(context->min_security_level == CRYPTO_SECURITY_LEVEL_256);
    
    crypto_context_destroy(context);
    
    printf("✓ Crypto context test passed\n");
}

void test_key_generation() {
    printf("Testing key generation...\n");
    
    CryptoKey* aes_key = crypto_key_generate(CRYPTO_EXTENDED_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256, "test");
    assert(aes_key != NULL);
    assert(aes_key->key_type == CRYPTO_KEY_SYMMETRIC);
    assert(aes_key->key_length == 32);
    
    CryptoKey* ed25519_key = crypto_key_generate(CRYPTO_EXTENDED_ED25519_SIGNATURE, CRYPTO_SECURITY_LEVEL_128, "signing");
    assert(ed25519_key != NULL);
    assert(ed25519_key->key_type == CRYPTO_KEY_ASYMMETRIC_PRIVATE);
    assert(ed25519_key->key_length == 32);
    
    crypto_key_destroy(ed25519_key);
    crypto_key_destroy(aes_key);
    
    printf("✓ Key generation test passed\n");
}

void test_crypto_policy() {
    printf("Testing crypto policy...\n");
    
    CryptoPolicy* policy = crypto_policy_create_default();
    assert(policy != NULL);
    assert(policy->min_security_level == CRYPTO_SECURITY_LEVEL_128);
    assert(policy->automatic_key_rotation == true);
    
    Result_void_ptr result = crypto_policy_validate_operation(policy, CRYPTO_EXTENDED_AES_256_GCM, CRYPTO_SECURITY_LEVEL_256);
    assert(!result.is_error);
    
    crypto_policy_destroy(policy);
    
    printf("✓ Crypto policy test passed\n");
}

int main() {
    printf("=== Cryptographic Security Simple Tests ===\n\n");
    
    test_algorithm_names();
    test_security_levels();
    test_random_generation();
    test_crypto_context();
    test_key_generation();
    test_crypto_policy();
    
    printf("\n=== All Crypto Security Tests Passed! ===\n");
    return 0;
}