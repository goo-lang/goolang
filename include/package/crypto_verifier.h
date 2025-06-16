#ifndef CRYPTO_VERIFIER_H
#define CRYPTO_VERIFIER_H

#include "ipfs_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Forward declarations
typedef struct CryptoVerifier CryptoVerifier;
typedef struct SigningKey SigningKey;
typedef struct PackageSignature PackageSignature;
typedef struct TrustChain TrustChain;
typedef struct SecurityPolicy SecurityPolicy;

// Cryptographic algorithms supported
typedef enum {
    CRYPTO_ALG_ED25519,         // Ed25519 (recommended)
    CRYPTO_ALG_RSA_2048,        // RSA 2048-bit
    CRYPTO_ALG_RSA_4096,        // RSA 4096-bit
    CRYPTO_ALG_ECDSA_P256,      // ECDSA P-256
    CRYPTO_ALG_ECDSA_P384,      // ECDSA P-384
    CRYPTO_ALG_SPHINCS_PLUS     // Post-quantum SPHINCS+
} CryptoAlgorithm;

// Hash algorithms for integrity
typedef enum {
    HASH_ALG_SHA256,            // SHA-256 (standard)
    HASH_ALG_SHA3_256,          // SHA3-256
    HASH_ALG_BLAKE3,            // BLAKE3 (fast)
    HASH_ALG_BLAKE2B            // BLAKE2b
} HashAlgorithm;

// Signing key with metadata
typedef struct SigningKey {
    char* key_id;               // Unique key identifier
    char* name;                 // Human-readable name
    CryptoAlgorithm algorithm;  // Cryptographic algorithm
    char* public_key;           // Public key (base64)
    char* private_key;          // Private key (encrypted, base64)
    
    // Key metadata
    time_t created_at;          // Creation timestamp
    time_t expires_at;          // Expiration timestamp (0 = never)
    char* issuer;               // Key issuer/organization
    char* email;                // Associated email
    char** capabilities;        // What this key can sign
    size_t capability_count;
    
    // Trust information
    int trust_level;            // Trust level (0-100)
    char** trusted_by;          // Who trusts this key
    size_t trusted_by_count;
    bool is_revoked;            // Is this key revoked
    time_t revoked_at;          // Revocation timestamp
    
    // Usage statistics
    int signature_count;        // Number of signatures made
    time_t last_used;          // Last usage timestamp
    
    struct SigningKey* next;    // Linked list
} SigningKey;

// Package signature with verification data
typedef struct PackageSignature {
    char* signature_id;         // Unique signature identifier
    char* package_name;         // Package being signed
    char* package_version;      // Package version
    IpfsCid* package_cid;       // Package content CID
    
    // Signature data
    CryptoAlgorithm algorithm;  // Signature algorithm
    HashAlgorithm hash_algorithm; // Hash algorithm used
    char* signature;            // Digital signature (base64)
    char* hash_value;           // Package hash value
    
    // Signer information
    char* signer_key_id;        // Signer's key ID
    char* signer_name;          // Signer's name
    char* signer_email;         // Signer's email
    char* signer_organization;  // Signer's organization
    
    // Timestamp information
    time_t signed_at;           // Signature timestamp
    char* timestamp_authority;  // Timestamp authority URL
    char* timestamp_signature;  // Timestamp authority signature
    
    // Additional metadata
    char* build_info;           // Build information
    char* commit_hash;          // Git commit hash
    char** dependencies;        // Dependency signatures
    size_t dependency_count;
    
    // Verification results
    bool is_verified;           // Has been verified
    bool is_valid;              // Verification result
    time_t verified_at;         // Verification timestamp
    char* verification_error;   // Verification error message
    
    struct PackageSignature* next; // Linked list
} PackageSignature;

// Trust chain for hierarchical verification
typedef struct TrustChain {
    SigningKey** keys;          // Chain of keys
    size_t key_count;
    PackageSignature** signatures; // Chain of signatures
    size_t signature_count;
    
    // Chain properties
    bool is_valid;              // Is the entire chain valid
    int trust_score;            // Overall trust score (0-100)
    char* root_authority;       // Root certificate authority
    time_t chain_built_at;      // When chain was built
    
    // Verification path
    char** verification_path;   // Path through trust chain
    size_t path_length;
} TrustChain;

// Security policy for verification
typedef struct SecurityPolicy {
    // Required signatures
    int min_signature_count;    // Minimum number of signatures
    char** required_signers;    // Required signer key IDs
    size_t required_signer_count;
    
    // Algorithm requirements
    CryptoAlgorithm* allowed_algorithms; // Allowed signature algorithms
    size_t allowed_algorithm_count;
    HashAlgorithm* allowed_hash_algorithms; // Allowed hash algorithms
    size_t allowed_hash_algorithm_count;
    
    // Trust requirements
    int min_trust_level;        // Minimum trust level
    bool require_timestamp;     // Require timestamp authority
    bool require_chain;         // Require full trust chain
    
    // Temporal requirements
    int max_signature_age;      // Max signature age (days)
    bool check_expiration;      // Check key expiration
    bool check_revocation;      // Check key revocation
    
    // Content requirements
    bool verify_dependencies;   // Verify dependency signatures
    bool require_build_info;    // Require build information
    bool require_reproducible;  // Require reproducible builds
    
    // Network security
    char** trusted_authorities; // Trusted timestamp authorities
    size_t authority_count;
    bool require_online_check;  // Require online revocation check
} SecurityPolicy;

// Crypto verifier instance
typedef struct CryptoVerifier {
    // Key management
    SigningKey* signing_keys;   // Available signing keys
    char* keystore_path;        // Path to keystore
    char* keystore_password;    // Keystore password (encrypted)
    
    // Signature storage
    PackageSignature* signatures; // Known signatures
    char* signature_cache_path; // Path to signature cache
    
    // Trust management
    TrustChain** trust_chains;  // Built trust chains
    size_t trust_chain_count;
    SigningKey** trusted_roots; // Trusted root keys
    size_t trusted_root_count;
    
    // Security configuration
    SecurityPolicy* policy;     // Current security policy
    bool strict_mode;           // Strict verification mode
    bool cache_verifications;   // Cache verification results
    
    // Performance optimization
    bool parallel_verification; // Verify signatures in parallel
    int verification_timeout;   // Verification timeout (seconds)
    size_t max_cache_size;      // Maximum cache size
} CryptoVerifier;

// Function declarations

// Crypto Verifier lifecycle
CryptoVerifier* crypto_verifier_create(void);
void crypto_verifier_free(CryptoVerifier* verifier);
bool crypto_verifier_initialize(CryptoVerifier* verifier);
bool crypto_verifier_load_keystore(CryptoVerifier* verifier, const char* keystore_path, 
                                  const char* password);
bool crypto_verifier_save_keystore(CryptoVerifier* verifier, const char* keystore_path);

// Key management
SigningKey* crypto_verifier_generate_key(CryptoVerifier* verifier, const char* name, 
                                        CryptoAlgorithm algorithm);
bool crypto_verifier_import_key(CryptoVerifier* verifier, const char* key_data, 
                               const char* name);
bool crypto_verifier_export_key(CryptoVerifier* verifier, const char* key_id, 
                               char** key_data_out);
bool crypto_verifier_delete_key(CryptoVerifier* verifier, const char* key_id);
SigningKey* crypto_verifier_get_key(CryptoVerifier* verifier, const char* key_id);
SigningKey** crypto_verifier_list_keys(CryptoVerifier* verifier, size_t* key_count);

// Package signing
PackageSignature* crypto_verifier_sign_package(CryptoVerifier* verifier, 
                                              const char* package_path,
                                              const char* key_id);
PackageSignature* crypto_verifier_sign_cid(CryptoVerifier* verifier, 
                                          const IpfsCid* cid,
                                          const char* package_name,
                                          const char* version,
                                          const char* key_id);
bool crypto_verifier_add_signature_metadata(PackageSignature* signature, 
                                           const char* build_info,
                                           const char* commit_hash);

// Package verification
bool crypto_verifier_verify_package(CryptoVerifier* verifier, const char* package_path);
bool crypto_verifier_verify_cid(CryptoVerifier* verifier, const IpfsCid* cid);
bool crypto_verifier_verify_signature(CryptoVerifier* verifier, 
                                     const PackageSignature* signature);
PackageSignature** crypto_verifier_get_signatures(CryptoVerifier* verifier, 
                                                 const IpfsCid* cid,
                                                 size_t* signature_count);

// Trust chain operations
TrustChain* crypto_verifier_build_trust_chain(CryptoVerifier* verifier, 
                                             const char* target_key_id);
bool crypto_verifier_verify_trust_chain(CryptoVerifier* verifier, 
                                       const TrustChain* chain);
int crypto_verifier_calculate_trust_score(CryptoVerifier* verifier, 
                                         const char* key_id);
bool crypto_verifier_add_trusted_root(CryptoVerifier* verifier, 
                                     const char* root_key_id);

// Security policy management
SecurityPolicy* crypto_verifier_create_policy(void);
void crypto_verifier_free_policy(SecurityPolicy* policy);
bool crypto_verifier_set_policy(CryptoVerifier* verifier, SecurityPolicy* policy);
bool crypto_verifier_check_policy_compliance(CryptoVerifier* verifier, 
                                            const PackageSignature* signature);

// Revocation and expiration
bool crypto_verifier_revoke_key(CryptoVerifier* verifier, const char* key_id, 
                               const char* reason);
bool crypto_verifier_check_revocation(CryptoVerifier* verifier, const char* key_id);
bool crypto_verifier_is_key_expired(CryptoVerifier* verifier, const char* key_id);
bool crypto_verifier_refresh_revocation_list(CryptoVerifier* verifier);

// Timestamp authority integration
bool crypto_verifier_add_timestamp(CryptoVerifier* verifier, PackageSignature* signature, 
                                  const char* authority_url);
bool crypto_verifier_verify_timestamp(CryptoVerifier* verifier, 
                                     const PackageSignature* signature);

// Key operations
SigningKey* signing_key_create(const char* name, CryptoAlgorithm algorithm);
void signing_key_free(SigningKey* key);
bool signing_key_generate_keypair(SigningKey* key);
bool signing_key_set_expiration(SigningKey* key, time_t expiration);
bool signing_key_add_capability(SigningKey* key, const char* capability);
char* signing_key_get_fingerprint(const SigningKey* key);

// Signature operations
PackageSignature* package_signature_create(const char* package_name, 
                                         const char* version,
                                         const IpfsCid* cid);
void package_signature_free(PackageSignature* signature);
bool package_signature_add_dependency(PackageSignature* signature, 
                                     const char* dependency_signature);
char* package_signature_serialize(const PackageSignature* signature);
PackageSignature* package_signature_deserialize(const char* data);

// Hash and crypto utilities
char* crypto_verifier_hash_file(const char* filepath, HashAlgorithm algorithm);
char* crypto_verifier_hash_data(const void* data, size_t size, 
                               HashAlgorithm algorithm);
char* crypto_verifier_hash_cid(const IpfsCid* cid, HashAlgorithm algorithm);
bool crypto_verifier_verify_hash(const char* expected_hash, const char* actual_hash);

// Reproducible build verification
typedef struct BuildInfo {
    char* compiler_version;     // Compiler version used
    char* build_timestamp;      // Build timestamp
    char* source_hash;          // Source code hash
    char* build_environment;    // Build environment info
    char** build_flags;         // Compiler flags used
    size_t flag_count;
} BuildInfo;

BuildInfo* crypto_verifier_extract_build_info(const char* package_path);
bool crypto_verifier_verify_reproducible_build(CryptoVerifier* verifier, 
                                              const PackageSignature* signature);
void build_info_free(BuildInfo* info);

// Multi-signature support
typedef struct MultiSignature {
    PackageSignature** signatures; // Multiple signatures
    size_t signature_count;
    int threshold;              // Required signature threshold
    bool is_valid;              // Are enough valid signatures present
} MultiSignature;

MultiSignature* crypto_verifier_create_multisig(CryptoVerifier* verifier, 
                                               const IpfsCid* cid,
                                               int threshold);
bool crypto_verifier_add_signature_to_multisig(MultiSignature* multisig, 
                                              PackageSignature* signature);
bool crypto_verifier_verify_multisig(CryptoVerifier* verifier, 
                                    const MultiSignature* multisig);
void multi_signature_free(MultiSignature* multisig);

// Blockchain integration (for transparency and immutability)
typedef struct BlockchainAnchor {
    char* blockchain;           // Blockchain name (ethereum, bitcoin, etc.)
    char* transaction_hash;     // Transaction hash
    char* smart_contract;       // Smart contract address
    uint64_t block_number;      // Block number
    time_t anchored_at;         // Anchor timestamp
} BlockchainAnchor;

bool crypto_verifier_anchor_to_blockchain(CryptoVerifier* verifier, 
                                         const PackageSignature* signature,
                                         const char* blockchain);
bool crypto_verifier_verify_blockchain_anchor(CryptoVerifier* verifier, 
                                             const BlockchainAnchor* anchor);

// Performance monitoring
typedef struct CryptoStats {
    size_t signatures_created;
    size_t signatures_verified;
    size_t verification_failures;
    float avg_signing_time;     // Average signing time (ms)
    float avg_verification_time; // Average verification time (ms)
    size_t cache_hits;
    size_t cache_misses;
    time_t stats_period_start;
} CryptoStats;

CryptoStats* crypto_verifier_get_stats(CryptoVerifier* verifier);
void crypto_stats_free(CryptoStats* stats);
bool crypto_verifier_reset_stats(CryptoVerifier* verifier);

// Configuration and persistence
typedef struct CryptoVerifierConfig {
    char* keystore_path;
    bool strict_mode;
    bool cache_verifications;
    bool parallel_verification;
    int verification_timeout;
    size_t max_cache_size;
    SecurityPolicy* default_policy;
} CryptoVerifierConfig;

CryptoVerifierConfig* crypto_verifier_config_create_default(void);
CryptoVerifierConfig* crypto_verifier_config_load(const char* config_file);
bool crypto_verifier_config_save(const CryptoVerifierConfig* config, 
                                const char* config_file);
void crypto_verifier_config_free(CryptoVerifierConfig* config);

#endif // CRYPTO_VERIFIER_H