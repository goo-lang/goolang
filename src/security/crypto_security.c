#include "../../include/crypto_security.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Secure memory management
static void* secure_memory_pool = NULL;
static size_t secure_memory_size = 0;
static bool secure_memory_initialized = false;

// Global crypto policy
static CryptoPolicy* global_crypto_policy = NULL;

// Key ID counter
static uint64_t key_id_counter = 1000;

// Utility functions
static uint64_t generate_key_id(void) {
    return __atomic_fetch_add(&key_id_counter, 1, __ATOMIC_SEQ_CST);
}

static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Secure memory operations
void crypto_secure_memzero(void* ptr, size_t size) {
    if (!ptr || size == 0) return;
    
    volatile uint8_t* volatile_ptr = (volatile uint8_t*)ptr;
    for (size_t i = 0; i < size; i++) {
        volatile_ptr[i] = 0;
    }
    
    // Memory barrier to prevent optimization
    __asm__ __volatile__("" ::: "memory");
}

void* crypto_secure_malloc(size_t size) {
    if (size == 0) return NULL;
    
    // Allocate with page alignment for better security
    void* ptr = aligned_alloc(4096, (size + 4095) & ~4095);
    if (!ptr) return NULL;
    
    // Lock pages in memory to prevent swapping
    if (mlock(ptr, size) != 0) {
        // If mlock fails, continue anyway but log warning
        fprintf(stderr, "Warning: Could not lock cryptographic memory pages\n");
    }
    
    return ptr;
}

void crypto_secure_free(void* ptr, size_t size) {
    if (!ptr) return;
    
    // Zero memory before freeing
    crypto_secure_memzero(ptr, size);
    
    // Unlock pages
    munlock(ptr, size);
    
    free(ptr);
}

bool crypto_memory_is_secure(const void* ptr, size_t size) {
    // Simple check - in a real implementation, this would check
    // if the memory is properly protected
    (void)ptr;
    (void)size;
    return secure_memory_initialized;
}

// Random number generation using /dev/urandom
Result_void_ptr crypto_random_bytes(uint8_t* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid parameters";
        }
        return ERR_PTR(err);
    }
    
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_CRYPTO_RANDOM_GENERATION_FAILED;
            err->message = "Failed to open /dev/urandom";
        }
        return ERR_PTR(err);
    }
    
    ssize_t bytes_read = 0;
    size_t total_read = 0;
    
    while (total_read < buffer_size) {
        bytes_read = read(fd, buffer + total_read, buffer_size - total_read);
        if (bytes_read < 0) {
            close(fd);
            Error* err = malloc(sizeof(Error));
            if (err) {
                err->code = ERROR_CRYPTO_RANDOM_GENERATION_FAILED;
                err->message = "Failed to read random bytes";
            }
            return ERR_PTR(err);
        }
        total_read += bytes_read;
    }
    
    close(fd);
    return OK_PTR(buffer);
}

uint32_t crypto_random_uint32(void) {
    uint32_t result;
    Result_void_ptr random_result = crypto_random_bytes((uint8_t*)&result, sizeof(result));
    if (random_result.is_error) {
        // Fallback to time-based pseudo-random
        result = (uint32_t)time(NULL) ^ (uint32_t)clock();
    }
    return result;
}

uint64_t crypto_random_uint64(void) {
    uint64_t result;
    Result_void_ptr random_result = crypto_random_bytes((uint8_t*)&result, sizeof(result));
    if (random_result.is_error) {
        // Fallback to time-based pseudo-random
        result = ((uint64_t)time(NULL) << 32) | (uint64_t)clock();
    }
    return result;
}

Result_void_ptr crypto_seed_entropy(const uint8_t* seed, size_t seed_length) {
    // In a real implementation, this would add entropy to the system
    // For now, just validate parameters
    if (!seed || seed_length == 0) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid seed parameters";
        }
        return ERR_PTR(err);
    }
    
    // Simulate seeding by writing to a temp file
    // (Real implementation would use proper entropy interfaces)
    return OK_PTR((void*)seed);
}

// Algorithm utility functions
const char* crypto_algorithm_name(CryptoAlgorithmExtended algorithm) {
    switch (algorithm) {
        case CRYPTO_EXTENDED_AES_256_GCM: return "AES-256-GCM";
        case CRYPTO_EXTENDED_AES_256_CBC: return "AES-256-CBC";
        case CRYPTO_EXTENDED_XCHACHA20_POLY1305: return "XChaCha20-Poly1305";
        case CRYPTO_EXTENDED_RSA_4096_OAEP: return "RSA-4096-OAEP";
        case CRYPTO_EXTENDED_RSA_3072_OAEP: return "RSA-3072-OAEP";
        case CRYPTO_EXTENDED_CURVE25519_X25519: return "Curve25519-X25519";
        case CRYPTO_EXTENDED_CURVE448_X448: return "Curve448-X448";
        case CRYPTO_EXTENDED_ED25519_SIGNATURE: return "Ed25519-Signature";
        case CRYPTO_EXTENDED_ED448_SIGNATURE: return "Ed448-Signature";
        case CRYPTO_EXTENDED_SHA3_256: return "SHA3-256";
        case CRYPTO_EXTENDED_SHA3_512: return "SHA3-512";
        case CRYPTO_EXTENDED_BLAKE3_HASH: return "BLAKE3";
        case CRYPTO_EXTENDED_ARGON2ID_HASH: return "Argon2id";
        case CRYPTO_EXTENDED_HKDF_SHA256: return "HKDF-SHA256";
        case CRYPTO_EXTENDED_HMAC_SHA3_256: return "HMAC-SHA3-256";
        default: return "Unknown";
    }
}

CryptoSecurityLevel crypto_algorithm_security_level(CryptoAlgorithmExtended algorithm) {
    switch (algorithm) {
        case CRYPTO_EXTENDED_AES_256_GCM:
        case CRYPTO_EXTENDED_AES_256_CBC:
        case CRYPTO_EXTENDED_XCHACHA20_POLY1305:
        case CRYPTO_EXTENDED_RSA_4096_OAEP:
        case CRYPTO_EXTENDED_CURVE448_X448:
        case CRYPTO_EXTENDED_ED448_SIGNATURE:
        case CRYPTO_EXTENDED_SHA3_512:
        case CRYPTO_EXTENDED_BLAKE3_HASH:
            return CRYPTO_SECURITY_LEVEL_256;
            
        case CRYPTO_EXTENDED_RSA_3072_OAEP:
            return CRYPTO_SECURITY_LEVEL_192;
            
        case CRYPTO_EXTENDED_CURVE25519_X25519:
        case CRYPTO_EXTENDED_ED25519_SIGNATURE:
        case CRYPTO_EXTENDED_SHA3_256:
        case CRYPTO_EXTENDED_HKDF_SHA256:
        case CRYPTO_EXTENDED_HMAC_SHA3_256:
            return CRYPTO_SECURITY_LEVEL_128;
            
        default:
            return CRYPTO_SECURITY_LEVEL_128;
    }
}

bool crypto_algorithm_is_post_quantum_safe(CryptoAlgorithmExtended algorithm) {
    switch (algorithm) {
        // Symmetric algorithms are generally post-quantum safe
        case CRYPTO_EXTENDED_AES_256_GCM:
        case CRYPTO_EXTENDED_AES_256_CBC:
        case CRYPTO_EXTENDED_XCHACHA20_POLY1305:
        case CRYPTO_EXTENDED_SHA3_256:
        case CRYPTO_EXTENDED_SHA3_512:
        case CRYPTO_EXTENDED_BLAKE3_HASH:
        case CRYPTO_EXTENDED_ARGON2ID_HASH:
        case CRYPTO_EXTENDED_HKDF_SHA256:
        case CRYPTO_EXTENDED_HMAC_SHA3_256:
            return true;
            
        // Current asymmetric algorithms are not post-quantum safe
        case CRYPTO_EXTENDED_RSA_4096_OAEP:
        case CRYPTO_EXTENDED_RSA_3072_OAEP:
        case CRYPTO_EXTENDED_CURVE25519_X25519:
        case CRYPTO_EXTENDED_CURVE448_X448:
        case CRYPTO_EXTENDED_ED25519_SIGNATURE:
        case CRYPTO_EXTENDED_ED448_SIGNATURE:
        case CRYPTO_EXTENDED_SECP256K1_ECDH:
        case CRYPTO_EXTENDED_SECP384R1_ECDH:
        case CRYPTO_EXTENDED_SECP256K1_ECDSA:
        case CRYPTO_EXTENDED_SECP384R1_ECDSA:
            return false;
            
        default:
            return false;
    }
}

bool crypto_algorithm_is_deprecated(CryptoAlgorithmExtended algorithm) {
    switch (algorithm) {
        // These algorithms are considered deprecated
        case CRYPTO_EXTENDED_AES_256_CBC:  // CBC mode vulnerable to padding oracle attacks
            return true;
            
        // All other algorithms are currently acceptable
        default:
            return false;
    }
}

// Crypto context operations
CryptoContext* crypto_context_create(CryptoAlgorithmExtended algorithm, CryptoSecurityLevel security_level) {
    CryptoContext* context = crypto_secure_malloc(sizeof(CryptoContext));
    if (!context) return NULL;
    
    memset(context, 0, sizeof(CryptoContext));
    
    context->algorithm = algorithm;
    context->min_security_level = security_level;
    context->auto_generate_nonce = true;
    context->audit_operations = true;
    context->hardware_acceleration = true;
    context->chunk_size = 64 * 1024; // 64KB default chunk size
    
    // Generate nonce buffer
    switch (algorithm) {
        case CRYPTO_EXTENDED_AES_256_GCM:
            context->nonce_length = 12; // 96-bit nonce for GCM
            break;
        case CRYPTO_EXTENDED_XCHACHA20_POLY1305:
            context->nonce_length = 24; // 192-bit nonce for XChaCha20
            break;
        default:
            context->nonce_length = 16; // 128-bit default
            break;
    }
    
    context->nonce = crypto_secure_malloc(context->nonce_length);
    if (!context->nonce) {
        crypto_secure_free(context, sizeof(CryptoContext));
        return NULL;
    }
    
    return context;
}

void crypto_context_destroy(CryptoContext* context) {
    if (!context) return;
    
    if (context->operation_state) {
        crypto_secure_free(context->operation_state, context->state_size);
    }
    
    if (context->associated_data) {
        crypto_secure_free(context->associated_data, context->associated_data_length);
    }
    
    if (context->nonce) {
        crypto_secure_free(context->nonce, context->nonce_length);
    }
    
    crypto_secure_free(context, sizeof(CryptoContext));
}

Result_void_ptr crypto_context_set_security_context(CryptoContext* context, SecurityContext* security_context) {
    if (!context) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid crypto context";
        }
        return ERR_PTR(err);
    }
    
    context->security_context = security_context;
    return OK_PTR(context);
}

// Key management operations
CryptoKey* crypto_key_generate(CryptoAlgorithmExtended algorithm, CryptoSecurityLevel security_level, const char* purpose) {
    CryptoKey* key = crypto_secure_malloc(sizeof(CryptoKey));
    if (!key) return NULL;
    
    memset(key, 0, sizeof(CryptoKey));
    
    key->key_id = generate_key_id();
    key->algorithm = algorithm;
    key->security_level = security_level;
    key->key_source = CRYPTO_KEY_SOURCE_GENERATED;
    key->created_at = time(NULL);
    key->expires_at = key->created_at + (CRYPTO_DEFAULT_KEY_LIFETIME_DAYS * 24 * 60 * 60);
    key->is_exportable = false;
    key->is_hardware_backed = false;
    
    // Determine key type and length based on algorithm
    switch (algorithm) {
        case CRYPTO_EXTENDED_AES_256_GCM:
        case CRYPTO_EXTENDED_AES_256_CBC:
        case CRYPTO_EXTENDED_XCHACHA20_POLY1305:
            key->key_type = CRYPTO_KEY_SYMMETRIC;
            key->key_length = 32; // 256 bits
            break;
            
        case CRYPTO_EXTENDED_CURVE25519_X25519:
        case CRYPTO_EXTENDED_ED25519_SIGNATURE:
            key->key_type = CRYPTO_KEY_ASYMMETRIC_PRIVATE;
            key->key_length = 32; // 256 bits
            break;
            
        case CRYPTO_EXTENDED_CURVE448_X448:
        case CRYPTO_EXTENDED_ED448_SIGNATURE:
            key->key_type = CRYPTO_KEY_ASYMMETRIC_PRIVATE;
            key->key_length = 57; // 448 bits
            break;
            
        case CRYPTO_EXTENDED_RSA_3072_OAEP:
            key->key_type = CRYPTO_KEY_ASYMMETRIC_PRIVATE;
            key->key_length = 384; // 3072 bits
            break;
            
        case CRYPTO_EXTENDED_RSA_4096_OAEP:
            key->key_type = CRYPTO_KEY_ASYMMETRIC_PRIVATE;
            key->key_length = 512; // 4096 bits
            break;
            
        default:
            crypto_secure_free(key, sizeof(CryptoKey));
            return NULL;
    }
    
    // Allocate key material
    key->key_material = crypto_secure_malloc(key->key_length);
    if (!key->key_material) {
        crypto_secure_free(key, sizeof(CryptoKey));
        return NULL;
    }
    
    // Generate random key material
    Result_void_ptr random_result = crypto_random_bytes(key->key_material, key->key_length);
    if (random_result.is_error) {
        crypto_secure_free(key->key_material, key->key_length);
        crypto_secure_free(key, sizeof(CryptoKey));
        return NULL;
    }
    
    // Set purpose and default capabilities
    if (purpose) {
        strncpy(key->purpose, purpose, sizeof(key->purpose) - 1);
        key->purpose[sizeof(key->purpose) - 1] = '\0';
    }
    
    // Set default key name
    snprintf(key->key_name, sizeof(key->key_name), "%s_key_%llu", 
             crypto_algorithm_name(algorithm), key->key_id);
    
    // Set default operations
    switch (key->key_type) {
        case CRYPTO_KEY_SYMMETRIC:
            strcpy(key->allowed_operations, "encrypt,decrypt,mac");
            break;
        case CRYPTO_KEY_ASYMMETRIC_PRIVATE:
            strcpy(key->allowed_operations, "decrypt,sign,key_exchange");
            break;
        case CRYPTO_KEY_ASYMMETRIC_PUBLIC:
            strcpy(key->allowed_operations, "encrypt,verify,key_exchange");
            break;
        default:
            strcpy(key->allowed_operations, "");
            break;
    }
    
    return key;
}

CryptoKeyPair* crypto_keypair_generate(CryptoAlgorithmExtended algorithm, CryptoSecurityLevel security_level, const char* purpose) {
    // Only generate keypairs for asymmetric algorithms
    switch (algorithm) {
        case CRYPTO_EXTENDED_RSA_3072_OAEP:
        case CRYPTO_EXTENDED_RSA_4096_OAEP:
        case CRYPTO_EXTENDED_CURVE25519_X25519:
        case CRYPTO_EXTENDED_CURVE448_X448:
        case CRYPTO_EXTENDED_ED25519_SIGNATURE:
        case CRYPTO_EXTENDED_ED448_SIGNATURE:
        case CRYPTO_EXTENDED_SECP256K1_ECDH:
        case CRYPTO_EXTENDED_SECP384R1_ECDH:
        case CRYPTO_EXTENDED_SECP256K1_ECDSA:
        case CRYPTO_EXTENDED_SECP384R1_ECDSA:
            break;
        default:
            return NULL; // Not an asymmetric algorithm
    }
    
    CryptoKeyPair* keypair = crypto_secure_malloc(sizeof(CryptoKeyPair));
    if (!keypair) return NULL;
    
    memset(keypair, 0, sizeof(CryptoKeyPair));
    
    // Generate private key
    keypair->private_key = crypto_key_generate(algorithm, security_level, purpose);
    if (!keypair->private_key) {
        crypto_secure_free(keypair, sizeof(CryptoKeyPair));
        return NULL;
    }
    
    // Generate corresponding public key
    keypair->public_key = crypto_secure_malloc(sizeof(CryptoKey));
    if (!keypair->public_key) {
        crypto_key_destroy(keypair->private_key);
        crypto_secure_free(keypair, sizeof(CryptoKeyPair));
        return NULL;
    }
    
    // Copy metadata from private key
    memcpy(keypair->public_key, keypair->private_key, sizeof(CryptoKey));
    
    // Adjust public key specifics
    keypair->public_key->key_id = generate_key_id();
    keypair->public_key->key_type = CRYPTO_KEY_ASYMMETRIC_PUBLIC;
    keypair->public_key->is_exportable = true; // Public keys can be exported
    strcpy(keypair->public_key->allowed_operations, "encrypt,verify,key_exchange");
    
    // For demonstration, copy the key material (in real implementation, derive public from private)
    keypair->public_key->key_material = crypto_secure_malloc(keypair->public_key->key_length);
    if (!keypair->public_key->key_material) {
        crypto_key_destroy(keypair->private_key);
        crypto_secure_free(keypair->public_key, sizeof(CryptoKey));
        crypto_secure_free(keypair, sizeof(CryptoKeyPair));
        return NULL;
    }
    
    // In a real implementation, this would derive the public key from the private key
    memcpy(keypair->public_key->key_material, keypair->private_key->key_material, keypair->public_key->key_length);
    
    keypair->created_at = time(NULL);
    
    if (purpose) {
        strncpy(keypair->pair_name, purpose, sizeof(keypair->pair_name) - 1);
        keypair->pair_name[sizeof(keypair->pair_name) - 1] = '\0';
    } else {
        snprintf(keypair->pair_name, sizeof(keypair->pair_name), "keypair_%llu", keypair->private_key->key_id);
    }
    
    return keypair;
}

void crypto_key_destroy(CryptoKey* key) {
    if (!key) return;
    
    if (key->key_material) {
        crypto_secure_free(key->key_material, key->key_length);
    }
    
    crypto_secure_free(key, sizeof(CryptoKey));
}

void crypto_keypair_destroy(CryptoKeyPair* keypair) {
    if (!keypair) return;
    
    crypto_key_destroy(keypair->private_key);
    crypto_key_destroy(keypair->public_key);
    crypto_secure_free(keypair, sizeof(CryptoKeyPair));
}

Result_void_ptr crypto_key_set_access_control(CryptoKey* key, SecurityCapability required_caps, const char* owner) {
    if (!key) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_CRYPTO_INVALID_KEY;
            err->message = "Invalid key";
        }
        return ERR_PTR(err);
    }
    
    key->required_capabilities = required_caps;
    
    if (owner) {
        strncpy(key->owner, owner, sizeof(key->owner) - 1);
        key->owner[sizeof(key->owner) - 1] = '\0';
    }
    
    return OK_PTR(key);
}

Result_void_ptr crypto_key_rotate(CryptoKey* key) {
    if (!key) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_CRYPTO_INVALID_KEY;
            err->message = "Invalid key";
        }
        return ERR_PTR(err);
    }
    
    // Store previous key ID
    key->previous_key_id = key->key_id;
    
    // Generate new key material
    Result_void_ptr random_result = crypto_random_bytes(key->key_material, key->key_length);
    if (random_result.is_error) {
        return random_result;
    }
    
    // Update metadata
    key->key_id = generate_key_id();
    key->rotation_count++;
    key->last_rotated = time(NULL);
    key->expires_at = key->last_rotated + (CRYPTO_DEFAULT_KEY_LIFETIME_DAYS * 24 * 60 * 60);
    key->usage_count = 0; // Reset usage counter
    
    return OK_PTR(key);
}

Result_bool crypto_key_is_valid(const CryptoKey* key) {
    if (!key) {
        return (Result_bool){.is_error = true, .value = false};
    }
    
    time_t now = time(NULL);
    bool is_valid = (now < key->expires_at) && (key->key_material != NULL);
    
    return (Result_bool){.is_error = false, .value = is_valid};
}

Result_bool crypto_key_can_perform_operation(const CryptoKey* key, const char* operation) {
    if (!key || !operation) {
        return (Result_bool){.is_error = true, .value = false};
    }
    
    // Check if key is valid first
    Result_bool valid_result = crypto_key_is_valid(key);
    if (valid_result.is_error || !valid_result.value) {
        return (Result_bool){.is_error = false, .value = false};
    }
    
    // Check if operation is allowed
    bool can_perform = (strstr(key->allowed_operations, operation) != NULL);
    
    return (Result_bool){.is_error = false, .value = can_perform};
}

// Result cleanup
void crypto_result_destroy(CryptoResult* result) {
    if (!result) return;
    
    if (result->data) {
        crypto_secure_free(result->data, result->data_length);
    }
    
    if (result->signature) {
        crypto_secure_free(result->signature, result->signature_length);
    }
    
    if (result->tag) {
        crypto_secure_free(result->tag, result->tag_length);
    }
    
    if (result->nonce_used) {
        crypto_secure_free(result->nonce_used, result->nonce_length);
    }
    
    if (result->error) {
        free(result->error);
    }
    
    free(result);
}

// Policy operations (stubs for now)
CryptoPolicy* crypto_policy_create_default(void) {
    CryptoPolicy* policy = malloc(sizeof(CryptoPolicy));
    if (!policy) return NULL;
    
    memset(policy, 0, sizeof(CryptoPolicy));
    
    strcpy(policy->policy_name, "Default Security Policy");
    strcpy(policy->version, "1.0");
    
    // Set preferred algorithms
    policy->preferred_symmetric_encryption[0] = CRYPTO_EXTENDED_AES_256_GCM;
    policy->preferred_symmetric_encryption[1] = CRYPTO_EXTENDED_XCHACHA20_POLY1305;
    policy->preferred_asymmetric_encryption[0] = CRYPTO_EXTENDED_CURVE25519_X25519;
    policy->preferred_asymmetric_encryption[1] = CRYPTO_EXTENDED_RSA_4096_OAEP;
    policy->preferred_signature_algorithms[0] = CRYPTO_EXTENDED_ED25519_SIGNATURE;
    policy->preferred_signature_algorithms[1] = CRYPTO_EXTENDED_RSA_4096_PSS_SHA256;
    policy->preferred_hash_algorithms[0] = CRYPTO_EXTENDED_SHA3_256;
    policy->preferred_hash_algorithms[1] = CRYPTO_EXTENDED_BLAKE3_HASH;
    
    policy->min_security_level = CRYPTO_SECURITY_LEVEL_128;
    policy->symmetric_key_lifetime_days = 365;
    policy->asymmetric_key_lifetime_days = 730;
    policy->key_rotation_interval_days = 90;
    policy->automatic_key_rotation = true;
    policy->audit_all_operations = true;
    policy->log_key_usage = true;
    
    return policy;
}

CryptoPolicy* crypto_policy_create_high_security(void) {
    CryptoPolicy* policy = crypto_policy_create_default();
    if (!policy) return NULL;
    
    strcpy(policy->policy_name, "High Security Policy");
    
    // Only allow strongest algorithms
    policy->preferred_symmetric_encryption[0] = CRYPTO_EXTENDED_AES_256_GCM;
    policy->preferred_asymmetric_encryption[0] = CRYPTO_EXTENDED_RSA_4096_OAEP;
    policy->preferred_signature_algorithms[0] = CRYPTO_EXTENDED_RSA_4096_PSS_SHA256;
    policy->preferred_hash_algorithms[0] = CRYPTO_EXTENDED_SHA3_512;
    
    policy->min_security_level = CRYPTO_SECURITY_LEVEL_256;
    policy->require_hardware_keys = true;
    policy->require_forward_secrecy = true;
    policy->fips_140_2_required = true;
    policy->symmetric_key_lifetime_days = 30;
    policy->asymmetric_key_lifetime_days = 365;
    policy->key_rotation_interval_days = 30;
    
    return policy;
}

void crypto_policy_destroy(CryptoPolicy* policy) {
    if (policy) {
        free(policy);
    }
}

Result_void_ptr crypto_policy_apply(CryptoPolicy* policy) {
    if (!policy) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid policy";
        }
        return ERR_PTR(err);
    }
    
    global_crypto_policy = policy;
    return OK_PTR(policy);
}

Result_void_ptr crypto_policy_validate_operation(const CryptoPolicy* policy, CryptoAlgorithmExtended algorithm, CryptoSecurityLevel security_level) {
    if (!policy) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "No policy specified";
        }
        return ERR_PTR(err);
    }
    
    // Check minimum security level
    if (security_level < policy->min_security_level) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_CRYPTO_INSUFFICIENT_SECURITY;
            err->message = "Security level below policy minimum";
        }
        return ERR_PTR(err);
    }
    
    // Check if algorithm is deprecated
    if (crypto_algorithm_is_deprecated(algorithm)) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_CRYPTO_POLICY_VIOLATION;
            err->message = "Algorithm is deprecated";
        }
        return ERR_PTR(err);
    }
    
    return OK_PTR((void*)policy);
}