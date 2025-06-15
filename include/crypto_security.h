#ifndef GOO_CRYPTO_SECURITY_H
#define GOO_CRYPTO_SECURITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>
#include "ergonomic_errors.h"

// Forward declarations for security types
typedef uint32_t SecurityCapability;
typedef struct SecurityContext SecurityContext;

#define CAP_CRYPTO_ENCRYPT     (1U << 20)
#define CAP_CRYPTO_DECRYPT     (1U << 21)
#define CAP_CRYPTO_SIGN        (1U << 22)
#define CAP_CRYPTO_VERIFY      (1U << 23)
#define CAP_CRYPTO_KEY_DERIVE  (1U << 24)
#define CAP_CRYPTO_BACKUP      (1U << 25)
#define CAP_CRYPTO_DESTROY     (1U << 26)

// Simple SecurityContext for standalone crypto module
struct SecurityContext {
    uint32_t policy_level;
    bool audit_enabled;
    char policy_name[64];
};

// Forward declarations
typedef struct CryptoContext CryptoContext;
typedef struct CryptoKey CryptoKey;
typedef struct CryptoKeyPair CryptoKeyPair;
typedef struct CryptoSession CryptoSession;
typedef struct CryptoPolicy CryptoPolicy;

// Extended cryptographic algorithm identifiers
typedef enum {
    // Extended symmetric encryption (starting at 2000 to avoid conflicts)
    CRYPTO_EXTENDED_AES_256_GCM = 2000,
    CRYPTO_EXTENDED_AES_256_CBC,
    CRYPTO_EXTENDED_XCHACHA20_POLY1305,
    
    // Extended asymmetric encryption
    CRYPTO_EXTENDED_RSA_4096_OAEP,
    CRYPTO_EXTENDED_RSA_3072_OAEP,
    CRYPTO_EXTENDED_CURVE25519_X25519,
    CRYPTO_EXTENDED_CURVE448_X448,
    CRYPTO_EXTENDED_SECP256K1_ECDH,
    CRYPTO_EXTENDED_SECP384R1_ECDH,
    
    // Extended digital signatures
    CRYPTO_EXTENDED_RSA_4096_PSS_SHA256,
    CRYPTO_EXTENDED_RSA_3072_PSS_SHA256,
    CRYPTO_EXTENDED_ED25519_SIGNATURE,
    CRYPTO_EXTENDED_ED448_SIGNATURE,
    CRYPTO_EXTENDED_SECP256K1_ECDSA,
    CRYPTO_EXTENDED_SECP384R1_ECDSA,
    
    // Extended hash functions
    CRYPTO_EXTENDED_SHA3_256,
    CRYPTO_EXTENDED_SHA3_512,
    CRYPTO_EXTENDED_BLAKE3_HASH,
    CRYPTO_EXTENDED_ARGON2ID_HASH,
    CRYPTO_EXTENDED_SCRYPT_HASH,
    
    // Extended key derivation
    CRYPTO_EXTENDED_HKDF_SHA256,
    CRYPTO_EXTENDED_PBKDF2_SHA256,
    CRYPTO_EXTENDED_ARGON2ID_KDF,
    
    // Extended message authentication
    CRYPTO_EXTENDED_HMAC_SHA3_256,
    CRYPTO_EXTENDED_HMAC_BLAKE3,
    CRYPTO_EXTENDED_POLY1305_MAC
} CryptoAlgorithmExtended;

// Cryptographic security levels
typedef enum {
    CRYPTO_SECURITY_LEVEL_128 = 128,  // 128-bit security
    CRYPTO_SECURITY_LEVEL_192 = 192,  // 192-bit security
    CRYPTO_SECURITY_LEVEL_256 = 256   // 256-bit security
} CryptoSecurityLevel;

// Key types
typedef enum {
    CRYPTO_KEY_SYMMETRIC,
    CRYPTO_KEY_ASYMMETRIC_PRIVATE,
    CRYPTO_KEY_ASYMMETRIC_PUBLIC,
    CRYPTO_KEY_DERIVED,
    CRYPTO_KEY_EPHEMERAL
} CryptoKeyType;

// Key sources
typedef enum {
    CRYPTO_KEY_SOURCE_GENERATED,
    CRYPTO_KEY_SOURCE_DERIVED,
    CRYPTO_KEY_SOURCE_IMPORTED,
    CRYPTO_KEY_SOURCE_HARDWARE,
    CRYPTO_KEY_SOURCE_ENVELOPE
} CryptoKeySource;

// Cryptographic key structure
typedef struct CryptoKey {
    uint64_t key_id;
    CryptoKeyType key_type;
    CryptoKeySource key_source;
    CryptoAlgorithmExtended algorithm;
    CryptoSecurityLevel security_level;
    
    // Key material (encrypted at rest)
    uint8_t* key_material;
    size_t key_length;
    
    // Key metadata
    char key_name[128];
    char purpose[64];
    time_t created_at;
    time_t expires_at;
    bool is_exportable;
    bool is_hardware_backed;
    
    // Access control
    SecurityCapability required_capabilities;
    char owner[64];
    char allowed_operations[256];
    
    // Key rotation
    uint32_t rotation_count;
    time_t last_rotated;
    uint64_t previous_key_id;
    
    // Usage tracking
    uint64_t usage_count;
    time_t last_used;
    
    struct CryptoKey* next;
} CryptoKey;

// Asymmetric key pair
typedef struct CryptoKeyPair {
    CryptoKey* private_key;
    CryptoKey* public_key;
    char pair_name[128];
    time_t created_at;
} CryptoKeyPair;

// Cryptographic operation context
typedef struct CryptoContext {
    CryptoAlgorithmExtended algorithm;
    CryptoSecurityLevel min_security_level;
    
    // Operation state
    uint8_t* operation_state;
    size_t state_size;
    bool is_initialized;
    
    // Associated data for AEAD
    uint8_t* associated_data;
    size_t associated_data_length;
    
    // Nonce/IV management
    uint8_t* nonce;
    size_t nonce_length;
    bool auto_generate_nonce;
    
    // Key references
    uint64_t encryption_key_id;
    uint64_t signing_key_id;
    
    // Security context integration
    SecurityContext* security_context;
    bool audit_operations;
    
    // Performance settings
    bool hardware_acceleration;
    bool parallel_processing;
    uint32_t chunk_size;
} CryptoContext;

// Cryptographic session for multi-step operations
typedef struct CryptoSession {
    uint64_t session_id;
    char session_name[128];
    
    // Session state
    time_t created_at;
    time_t last_activity;
    time_t expires_at;
    bool is_active;
    
    // Associated crypto context
    CryptoContext* crypto_context;
    
    // Session keys (ephemeral)
    CryptoKey* session_key;
    CryptoKey* authentication_key;
    
    // Multi-party session support
    char participants[10][64];
    size_t participant_count;
    
    // Session security
    bool requires_authentication;
    bool forward_secrecy;
    uint32_t ratchet_counter;
    
    struct CryptoSession* next;
} CryptoSession;

// Cryptographic policy for organization-wide defaults
typedef struct CryptoPolicy {
    char policy_name[128];
    char version[32];
    
    // Algorithm preferences (ordered by preference)
    CryptoAlgorithmExtended preferred_symmetric_encryption[5];
    CryptoAlgorithmExtended preferred_asymmetric_encryption[5];
    CryptoAlgorithmExtended preferred_signature_algorithms[5];
    CryptoAlgorithmExtended preferred_hash_algorithms[5];
    
    // Security requirements
    CryptoSecurityLevel min_security_level;
    bool require_hardware_keys;
    bool require_forward_secrecy;
    bool require_post_quantum_ready;
    
    // Key management policies
    uint32_t symmetric_key_lifetime_days;
    uint32_t asymmetric_key_lifetime_days;
    uint32_t key_rotation_interval_days;
    bool automatic_key_rotation;
    
    // Compliance requirements
    bool fips_140_2_required;
    bool common_criteria_required;
    char compliance_frameworks[256];
    
    // Operational policies
    bool allow_key_export;
    bool require_key_escrow;
    bool enable_key_recovery;
    char backup_locations[512];
    
    // Audit and monitoring
    bool audit_all_operations;
    bool log_key_usage;
    bool monitor_anomalies;
    char audit_destination[256];
} CryptoPolicy;

// Cryptographic operation result
typedef struct CryptoResult {
    bool success;
    Error* error;
    
    // Operation output
    uint8_t* data;
    size_t data_length;
    
    // Metadata
    CryptoAlgorithmExtended algorithm_used;
    uint64_t key_id_used;
    time_t operation_time;
    uint64_t operation_duration_ns;
    
    // Verification data
    uint8_t* signature;
    size_t signature_length;
    uint8_t* tag;
    size_t tag_length;
    
    // Nonce/IV used
    uint8_t* nonce_used;
    size_t nonce_length;
} CryptoResult;

// Core cryptographic operations
CryptoContext* crypto_context_create(CryptoAlgorithmExtended algorithm, CryptoSecurityLevel security_level);
void crypto_context_destroy(CryptoContext* context);
Result_void_ptr crypto_context_set_security_context(CryptoContext* context, SecurityContext* security_context);

// Key management operations
CryptoKey* crypto_key_generate(CryptoAlgorithmExtended algorithm, CryptoSecurityLevel security_level, const char* purpose);
CryptoKeyPair* crypto_keypair_generate(CryptoAlgorithmExtended algorithm, CryptoSecurityLevel security_level, const char* purpose);
void crypto_key_destroy(CryptoKey* key);
void crypto_keypair_destroy(CryptoKeyPair* keypair);

Result_void_ptr crypto_key_set_access_control(CryptoKey* key, SecurityCapability required_caps, const char* owner);
Result_void_ptr crypto_key_rotate(CryptoKey* key);
Result_bool crypto_key_is_valid(const CryptoKey* key);
Result_bool crypto_key_can_perform_operation(const CryptoKey* key, const char* operation);

// Key derivation
CryptoKey* crypto_key_derive(const CryptoKey* master_key, const char* purpose, const uint8_t* salt, size_t salt_length);
CryptoKey* crypto_key_derive_from_password(const char* password, const uint8_t* salt, size_t salt_length, uint32_t iterations);

// Symmetric encryption operations
CryptoResult* crypto_encrypt_symmetric(CryptoContext* context, const CryptoKey* key, const uint8_t* plaintext, size_t plaintext_length);
CryptoResult* crypto_decrypt_symmetric(CryptoContext* context, const CryptoKey* key, const uint8_t* ciphertext, size_t ciphertext_length);

// Asymmetric encryption operations
CryptoResult* crypto_encrypt_asymmetric(CryptoContext* context, const CryptoKey* public_key, const uint8_t* plaintext, size_t plaintext_length);
CryptoResult* crypto_decrypt_asymmetric(CryptoContext* context, const CryptoKey* private_key, const uint8_t* ciphertext, size_t ciphertext_length);

// Digital signature operations
CryptoResult* crypto_sign(CryptoContext* context, const CryptoKey* private_key, const uint8_t* message, size_t message_length);
Result_bool crypto_verify_signature(CryptoContext* context, const CryptoKey* public_key, const uint8_t* message, size_t message_length, const uint8_t* signature, size_t signature_length);

// Hash operations
CryptoResult* crypto_hash(CryptoAlgorithmExtended algorithm, const uint8_t* data, size_t data_length);
CryptoResult* crypto_hmac(const CryptoKey* key, const uint8_t* data, size_t data_length);

// Key exchange operations
CryptoResult* crypto_key_exchange(CryptoContext* context, const CryptoKey* private_key, const CryptoKey* peer_public_key);

// Session management
CryptoSession* crypto_session_create(const char* session_name, CryptoContext* context, time_t session_lifetime);
void crypto_session_destroy(CryptoSession* session);
Result_void_ptr crypto_session_add_participant(CryptoSession* session, const char* participant_id);
Result_void_ptr crypto_session_establish_keys(CryptoSession* session);

// High-level secure operations (convenience functions)
CryptoResult* crypto_secure_encrypt(const uint8_t* plaintext, size_t plaintext_length, const char* recipient_id);
CryptoResult* crypto_secure_decrypt(const uint8_t* ciphertext, size_t ciphertext_length, const char* sender_id);
CryptoResult* crypto_secure_sign(const uint8_t* message, size_t message_length, const char* signer_id);
Result_bool crypto_secure_verify(const uint8_t* message, size_t message_length, const uint8_t* signature, size_t signature_length, const char* signer_id);

// Policy management
CryptoPolicy* crypto_policy_create_default(void);
CryptoPolicy* crypto_policy_create_high_security(void);
CryptoPolicy* crypto_policy_create_performance_optimized(void);
CryptoPolicy* crypto_policy_create_compliance(const char* framework);
void crypto_policy_destroy(CryptoPolicy* policy);

Result_void_ptr crypto_policy_apply(CryptoPolicy* policy);
Result_void_ptr crypto_policy_validate_operation(const CryptoPolicy* policy, CryptoAlgorithmExtended algorithm, CryptoSecurityLevel security_level);

// Key store operations
typedef struct CryptoKeyStore {
    char store_name[128];
    char store_path[512];
    
    // Store configuration
    bool encrypted_at_rest;
    bool hardware_backed;
    CryptoKey* master_key;
    
    // Key storage
    CryptoKey* keys;
    size_t key_count;
    size_t max_keys;
    
    // Access control
    SecurityContext* security_context;
    bool audit_access;
    
    // Backup and recovery
    char backup_path[512];
    bool auto_backup;
    time_t last_backup;
} CryptoKeyStore;

CryptoKeyStore* crypto_keystore_create(const char* store_name, const char* store_path);
void crypto_keystore_destroy(CryptoKeyStore* store);
Result_void_ptr crypto_keystore_add_key(CryptoKeyStore* store, CryptoKey* key);
CryptoKey* crypto_keystore_get_key(CryptoKeyStore* store, uint64_t key_id);
CryptoKey* crypto_keystore_find_key(CryptoKeyStore* store, const char* key_name);
Result_void_ptr crypto_keystore_remove_key(CryptoKeyStore* store, uint64_t key_id);
Result_void_ptr crypto_keystore_backup(CryptoKeyStore* store);
Result_void_ptr crypto_keystore_restore(CryptoKeyStore* store, const char* backup_path);

// Memory security functions
void crypto_secure_memzero(void* ptr, size_t size);
void* crypto_secure_malloc(size_t size);
void crypto_secure_free(void* ptr, size_t size);
bool crypto_memory_is_secure(const void* ptr, size_t size);

// Random number generation
Result_void_ptr crypto_random_bytes(uint8_t* buffer, size_t buffer_size);
uint32_t crypto_random_uint32(void);
uint64_t crypto_random_uint64(void);
Result_void_ptr crypto_seed_entropy(const uint8_t* seed, size_t seed_length);

// Utility functions
const char* crypto_algorithm_name(CryptoAlgorithmExtended algorithm);
CryptoSecurityLevel crypto_algorithm_security_level(CryptoAlgorithmExtended algorithm);
bool crypto_algorithm_is_post_quantum_safe(CryptoAlgorithmExtended algorithm);
bool crypto_algorithm_is_deprecated(CryptoAlgorithmExtended algorithm);

// Error cleanup
void crypto_result_destroy(CryptoResult* result);

// Configuration constants
#define CRYPTO_DEFAULT_SYMMETRIC_ALGORITHM      CRYPTO_EXTENDED_AES_256_GCM
#define CRYPTO_DEFAULT_ASYMMETRIC_ALGORITHM     CRYPTO_EXTENDED_CURVE25519_X25519
#define CRYPTO_DEFAULT_SIGNATURE_ALGORITHM      CRYPTO_EXTENDED_ED25519_SIGNATURE
#define CRYPTO_DEFAULT_HASH_ALGORITHM           CRYPTO_EXTENDED_SHA3_256
#define CRYPTO_DEFAULT_SECURITY_LEVEL           CRYPTO_SECURITY_LEVEL_256

#define CRYPTO_DEFAULT_KEY_LIFETIME_DAYS        365
#define CRYPTO_DEFAULT_SESSION_LIFETIME_HOURS   24
#define CRYPTO_DEFAULT_ROTATION_INTERVAL_DAYS   90


// Cryptographic error codes
#define ERROR_CRYPTO_ALGORITHM_NOT_SUPPORTED    0x9001
#define ERROR_CRYPTO_INVALID_KEY               0x9002
#define ERROR_CRYPTO_KEY_EXPIRED               0x9003
#define ERROR_CRYPTO_INSUFFICIENT_SECURITY     0x9004
#define ERROR_CRYPTO_OPERATION_FAILED_EXT      0x9005
#define ERROR_CRYPTO_VERIFICATION_FAILED       0x9006
#define ERROR_CRYPTO_POLICY_VIOLATION          0x9007
#define ERROR_CRYPTO_HARDWARE_NOT_AVAILABLE    0x9008
#define ERROR_CRYPTO_RANDOM_GENERATION_FAILED  0x9009
#define ERROR_CRYPTO_KEY_STORE_FULL            0x900A

#endif // GOO_CRYPTO_SECURITY_H