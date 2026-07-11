#include "package/crypto_verifier.h"
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include <json-c/json.h>
#include <pthread.h>

// Thread-safe statistics
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Generate unique ID for signatures and keys
static char* generate_unique_id(void) {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return NULL;
    
    char* id = malloc(33);
    if (!id) return NULL;
    
    for (int i = 0; i < 16; i++) {
        sprintf(id + i * 2, "%02x", bytes[i]);
    }
    id[32] = '\0';
    
    return id;
}

// Hash data using specified algorithm
static char* hash_data_internal(const void* data, size_t size, HashAlgorithm algorithm) {
    if (!data || size == 0) return NULL;
    
    unsigned char hash[64]; // Max hash size (SHA-512)
    unsigned int hash_len = 0;
    
    switch (algorithm) {
        case HASH_ALG_SHA256: {
            SHA256_CTX ctx;
            SHA256_Init(&ctx);
            SHA256_Update(&ctx, data, size);
            SHA256_Final(hash, &ctx);
            hash_len = SHA256_DIGEST_LENGTH;
            break;
        }
        case HASH_ALG_SHA3_256:
        case HASH_ALG_BLAKE3:
        case HASH_ALG_BLAKE2B:
            // For now, fallback to SHA256
            SHA256_CTX ctx;
            SHA256_Init(&ctx);
            SHA256_Update(&ctx, data, size);
            SHA256_Final(hash, &ctx);
            hash_len = SHA256_DIGEST_LENGTH;
            break;
        default:
            return NULL;
    }
    
    // Convert to hex string
    char* hex_hash = malloc(hash_len * 2 + 1);
    if (!hex_hash) return NULL;
    
    for (unsigned int i = 0; i < hash_len; i++) {
        sprintf(hex_hash + i * 2, "%02x", hash[i]);
    }
    hex_hash[hash_len * 2] = '\0';
    
    return hex_hash;
}

CryptoVerifier* crypto_verifier_create(void) {
    CryptoVerifier* verifier = xcalloc(1, sizeof(CryptoVerifier));
    if (!verifier) return NULL;
    
    verifier->policy = crypto_verifier_create_policy();
    if (!verifier->policy) {
        free(verifier);
        return NULL;
    }
    
    // Default settings
    verifier->strict_mode = false;
    verifier->cache_verifications = true;
    verifier->parallel_verification = true;
    verifier->verification_timeout = 30;
    verifier->max_cache_size = 10000;
    
    return verifier;
}

void crypto_verifier_free(CryptoVerifier* verifier) {
    if (!verifier) return;
    
    // Free signing keys
    SigningKey* key = verifier->signing_keys;
    while (key) {
        SigningKey* next = key->next;
        signing_key_free(key);
        key = next;
    }
    
    // Free signatures
    PackageSignature* sig = verifier->signatures;
    while (sig) {
        PackageSignature* next = sig->next;
        package_signature_free(sig);
        sig = next;
    }
    
    // Free trust chains
    for (size_t i = 0; i < verifier->trust_chain_count; i++) {
        if (verifier->trust_chains[i]) {
            free(verifier->trust_chains[i]->keys);
            free(verifier->trust_chains[i]->signatures);
            free(verifier->trust_chains[i]->verification_path);
            free(verifier->trust_chains[i]);
        }
    }
    free(verifier->trust_chains);
    
    // Free trusted roots
    for (size_t i = 0; i < verifier->trusted_root_count; i++) {
        signing_key_free(verifier->trusted_roots[i]);
    }
    free(verifier->trusted_roots);
    
    crypto_verifier_free_policy(verifier->policy);
    free(verifier->keystore_path);
    free(verifier->keystore_password);
    free(verifier->signature_cache_path);
    free(verifier);
}

bool crypto_verifier_initialize(CryptoVerifier* verifier) {
    if (!verifier) return false;
    
    // Initialize OpenSSL
    OpenSSL_add_all_algorithms();
    
    // Seed random number generator
    if (RAND_load_file("/dev/urandom", 32) <= 0) {
        // Fallback for systems without /dev/urandom
        unsigned char seed[32];
        for (int i = 0; i < 32; i++) {
            seed[i] = (unsigned char)(rand() % 256);
        }
        RAND_seed(seed, sizeof(seed));
    }
    
    return true;
}

SigningKey* crypto_verifier_generate_key(CryptoVerifier* verifier, const char* name, 
                                        CryptoAlgorithm algorithm) {
    if (!verifier || !name) return NULL;
    
    SigningKey* key = signing_key_create(name, algorithm);
    if (!key) return NULL;
    
    if (!signing_key_generate_keypair(key)) {
        signing_key_free(key);
        return NULL;
    }
    
    // Add to verifier's key list
    key->next = verifier->signing_keys;
    verifier->signing_keys = key;
    
    return key;
}

SigningKey* crypto_verifier_get_key(CryptoVerifier* verifier, const char* key_id) {
    if (!verifier || !key_id) return NULL;
    
    SigningKey* key = verifier->signing_keys;
    while (key) {
        if (strcmp(key->key_id, key_id) == 0) {
            return key;
        }
        key = key->next;
    }
    
    return NULL;
}

PackageSignature* crypto_verifier_sign_cid(CryptoVerifier* verifier, 
                                          const IpfsCid* cid,
                                          const char* package_name,
                                          const char* version,
                                          const char* key_id) {
    if (!verifier || !cid || !package_name || !version || !key_id) return NULL;
    
    SigningKey* key = crypto_verifier_get_key(verifier, key_id);
    if (!key) return NULL;
    
    PackageSignature* signature = package_signature_create(package_name, version, cid);
    if (!signature) return NULL;
    
    // Set signature metadata
    signature->algorithm = key->algorithm;
    signature->hash_algorithm = HASH_ALG_SHA256; // Default
    signature->signer_key_id = strdup(key->key_id);
    signature->signer_name = strdup(key->name);
    signature->signer_email = key->email ? strdup(key->email) : NULL;
    signature->signed_at = time(NULL);
    
    // Hash the CID
    signature->hash_value = crypto_verifier_hash_cid(cid, signature->hash_algorithm);
    if (!signature->hash_value) {
        package_signature_free(signature);
        return NULL;
    }
    
    // Create signature data (simplified - would use proper cryptographic signing)
    char* sign_data = malloc(strlen(signature->hash_value) + strlen(key->key_id) + 32);
    if (!sign_data) {
        package_signature_free(signature);
        return NULL;
    }
    
    sprintf(sign_data, "%s:%s:%ld", signature->hash_value, key->key_id, signature->signed_at);
    
    // Generate signature (simplified - would use proper cryptographic algorithms)
    signature->signature = hash_data_internal(sign_data, strlen(sign_data), HASH_ALG_SHA256);
    free(sign_data);
    
    if (!signature->signature) {
        package_signature_free(signature);
        return NULL;
    }
    
    // Add to verifier's signature list
    signature->next = verifier->signatures;
    verifier->signatures = signature;
    
    // Update key usage
    key->signature_count++;
    key->last_used = time(NULL);
    
    return signature;
}

bool crypto_verifier_verify_signature(CryptoVerifier* verifier, 
                                     const PackageSignature* signature) {
    if (!verifier || !signature) return false;
    
    // Get the signing key
    SigningKey* key = crypto_verifier_get_key(verifier, signature->signer_key_id);
    if (!key) return false;
    
    // Check if key is revoked or expired
    if (key->is_revoked) return false;
    if (key->expires_at > 0 && time(NULL) > key->expires_at) return false;
    
    // Check policy compliance
    if (!crypto_verifier_check_policy_compliance(verifier, signature)) {
        return false;
    }
    
    // Verify signature (simplified - would use proper cryptographic verification)
    char* sign_data = malloc(strlen(signature->hash_value) + strlen(key->key_id) + 32);
    if (!sign_data) return false;
    
    sprintf(sign_data, "%s:%s:%ld", signature->hash_value, key->key_id, signature->signed_at);
    
    char* expected_signature = hash_data_internal(sign_data, strlen(sign_data), HASH_ALG_SHA256);
    free(sign_data);
    
    if (!expected_signature) return false;
    
    bool valid = (strcmp(signature->signature, expected_signature) == 0);
    free(expected_signature);
    
    return valid;
}

bool crypto_verifier_verify_cid(CryptoVerifier* verifier, const IpfsCid* cid) {
    if (!verifier || !cid) return false;
    
    // Find signatures for this CID
    size_t signature_count;
    PackageSignature** signatures = crypto_verifier_get_signatures(verifier, cid, &signature_count);
    
    if (!signatures || signature_count == 0) {
        free(signatures);
        return false;
    }
    
    bool any_valid = false;
    for (size_t i = 0; i < signature_count; i++) {
        if (crypto_verifier_verify_signature(verifier, signatures[i])) {
            any_valid = true;
            break;
        }
    }
    
    free(signatures);
    return any_valid;
}

PackageSignature** crypto_verifier_get_signatures(CryptoVerifier* verifier, 
                                                 const IpfsCid* cid,
                                                 size_t* signature_count) {
    if (!verifier || !cid || !signature_count) return NULL;
    
    *signature_count = 0;
    
    // Count matching signatures
    PackageSignature* sig = verifier->signatures;
    while (sig) {
        if (ipfs_cid_equals(sig->package_cid, cid)) {
            (*signature_count)++;
        }
        sig = sig->next;
    }
    
    if (*signature_count == 0) return NULL;
    
    PackageSignature** result = calloc(*signature_count, sizeof(PackageSignature*));
    if (!result) return NULL;
    
    // Collect matching signatures
    size_t index = 0;
    sig = verifier->signatures;
    while (sig && index < *signature_count) {
        if (ipfs_cid_equals(sig->package_cid, cid)) {
            result[index++] = sig;
        }
        sig = sig->next;
    }
    
    return result;
}

SecurityPolicy* crypto_verifier_create_policy(void) {
    SecurityPolicy* policy = xcalloc(1, sizeof(SecurityPolicy));
    if (!policy) return NULL;
    
    // Default policy settings
    policy->min_signature_count = 1;
    policy->min_trust_level = 50;
    policy->require_timestamp = false;
    policy->require_chain = false;
    policy->max_signature_age = 365; // 1 year
    policy->check_expiration = true;
    policy->check_revocation = true;
    policy->verify_dependencies = true;
    policy->require_build_info = false;
    policy->require_reproducible = false;
    policy->require_online_check = false;
    
    // Default allowed algorithms
    policy->allowed_algorithm_count = 2;
    policy->allowed_algorithms = malloc(policy->allowed_algorithm_count * sizeof(CryptoAlgorithm));
    if (policy->allowed_algorithms) {
        policy->allowed_algorithms[0] = CRYPTO_ALG_ED25519;
        policy->allowed_algorithms[1] = CRYPTO_ALG_RSA_4096;
    }
    
    return policy;
}

void crypto_verifier_free_policy(SecurityPolicy* policy) {
    if (!policy) return;
    
    for (size_t i = 0; i < policy->required_signer_count; i++) {
        free(policy->required_signers[i]);
    }
    free(policy->required_signers);
    
    free(policy->allowed_algorithms);
    free(policy->allowed_hash_algorithms);
    
    for (size_t i = 0; i < policy->authority_count; i++) {
        free(policy->trusted_authorities[i]);
    }
    free(policy->trusted_authorities);
    
    free(policy);
}

bool crypto_verifier_check_policy_compliance(CryptoVerifier* verifier, 
                                            const PackageSignature* signature) {
    if (!verifier || !signature || !verifier->policy) return false;
    
    SecurityPolicy* policy = verifier->policy;
    
    // Check algorithm compliance
    bool algorithm_allowed = false;
    for (size_t i = 0; i < policy->allowed_algorithm_count; i++) {
        if (policy->allowed_algorithms[i] == signature->algorithm) {
            algorithm_allowed = true;
            break;
        }
    }
    if (!algorithm_allowed) return false;
    
    // Check signature age
    if (policy->max_signature_age > 0) {
        time_t max_age = time(NULL) - (policy->max_signature_age * 24 * 3600);
        if (signature->signed_at < max_age) return false;
    }
    
    // Check if timestamp is required
    if (policy->require_timestamp && !signature->timestamp_authority) {
        return false;
    }
    
    // Check trust level
    SigningKey* key = crypto_verifier_get_key(verifier, signature->signer_key_id);
    if (key && key->trust_level < policy->min_trust_level) {
        return false;
    }
    
    return true;
}

// Key operations
SigningKey* signing_key_create(const char* name, CryptoAlgorithm algorithm) {
    if (!name) return NULL;
    
    SigningKey* key = xcalloc(1, sizeof(SigningKey));
    if (!key) return NULL;
    
    key->key_id = generate_unique_id();
    key->name = strdup(name);
    key->algorithm = algorithm;
    key->created_at = time(NULL);
    key->trust_level = 50; // Default trust level
    
    if (!key->key_id || !key->name) {
        signing_key_free(key);
        return NULL;
    }
    
    return key;
}

void signing_key_free(SigningKey* key) {
    if (!key) return;
    
    free(key->key_id);
    free(key->name);
    free(key->public_key);
    free(key->private_key);
    free(key->issuer);
    free(key->email);
    
    for (size_t i = 0; i < key->capability_count; i++) {
        free(key->capabilities[i]);
    }
    free(key->capabilities);
    
    for (size_t i = 0; i < key->trusted_by_count; i++) {
        free(key->trusted_by[i]);
    }
    free(key->trusted_by);
    
    free(key);
}

bool signing_key_generate_keypair(SigningKey* key) {
    if (!key) return false;
    
    switch (key->algorithm) {
        case CRYPTO_ALG_ED25519: {
            EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
            if (!ctx) return false;
            
            if (EVP_PKEY_keygen_init(ctx) <= 0) {
                EVP_PKEY_CTX_free(ctx);
                return false;
            }
            
            EVP_PKEY* pkey = NULL;
            if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
                EVP_PKEY_CTX_free(ctx);
                return false;
            }
            
            EVP_PKEY_CTX_free(ctx);
            
            // Extract keys (simplified)
            key->public_key = strdup("ed25519_public_key_placeholder");
            key->private_key = strdup("ed25519_private_key_placeholder");
            
            EVP_PKEY_free(pkey);
            break;
        }
        
        case CRYPTO_ALG_RSA_2048:
        case CRYPTO_ALG_RSA_4096: {
            int bits = (key->algorithm == CRYPTO_ALG_RSA_2048) ? 2048 : 4096;
            
            EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
            if (!ctx) return false;
            
            if (EVP_PKEY_keygen_init(ctx) <= 0) {
                EVP_PKEY_CTX_free(ctx);
                return false;
            }
            
            if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0) {
                EVP_PKEY_CTX_free(ctx);
                return false;
            }
            
            EVP_PKEY* pkey = NULL;
            if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
                EVP_PKEY_CTX_free(ctx);
                return false;
            }
            
            EVP_PKEY_CTX_free(ctx);
            
            // Extract keys (simplified)
            key->public_key = strdup("rsa_public_key_placeholder");
            key->private_key = strdup("rsa_private_key_placeholder");
            
            EVP_PKEY_free(pkey);
            break;
        }
        
        default:
            return false;
    }
    
    return (key->public_key && key->private_key);
}

// Signature operations
PackageSignature* package_signature_create(const char* package_name, 
                                         const char* version,
                                         const IpfsCid* cid) {
    if (!package_name || !version || !cid) return NULL;
    
    PackageSignature* signature = xcalloc(1, sizeof(PackageSignature));
    if (!signature) return NULL;
    
    signature->signature_id = generate_unique_id();
    signature->package_name = strdup(package_name);
    signature->package_version = strdup(version);
    signature->package_cid = ipfs_cid_clone(cid);
    
    if (!signature->signature_id || !signature->package_name || 
        !signature->package_version || !signature->package_cid) {
        package_signature_free(signature);
        return NULL;
    }
    
    return signature;
}

void package_signature_free(PackageSignature* signature) {
    if (!signature) return;
    
    free(signature->signature_id);
    free(signature->package_name);
    free(signature->package_version);
    ipfs_cid_free(signature->package_cid);
    free(signature->signature);
    free(signature->hash_value);
    free(signature->signer_key_id);
    free(signature->signer_name);
    free(signature->signer_email);
    free(signature->signer_organization);
    free(signature->timestamp_authority);
    free(signature->timestamp_signature);
    free(signature->build_info);
    free(signature->commit_hash);
    free(signature->verification_error);
    
    for (size_t i = 0; i < signature->dependency_count; i++) {
        free(signature->dependencies[i]);
    }
    free(signature->dependencies);
    
    free(signature);
}

// Hash utilities
char* crypto_verifier_hash_data(const void* data, size_t size, HashAlgorithm algorithm) {
    return hash_data_internal(data, size, algorithm);
}

char* crypto_verifier_hash_cid(const IpfsCid* cid, HashAlgorithm algorithm) {
    if (!cid || !cid->hash) return NULL;
    
    return hash_data_internal(cid->hash, strlen(cid->hash), algorithm);
}

bool crypto_verifier_verify_hash(const char* expected_hash, const char* actual_hash) {
    if (!expected_hash || !actual_hash) return false;
    
    return strcmp(expected_hash, actual_hash) == 0;
}

// Statistics
CryptoStats* crypto_verifier_get_stats(CryptoVerifier* verifier) {
    if (!verifier) return NULL;
    
    CryptoStats* stats = xcalloc(1, sizeof(CryptoStats));
    if (!stats) return NULL;
    
    pthread_mutex_lock(&stats_mutex);
    
    // Count signatures created and verified
    PackageSignature* sig = verifier->signatures;
    while (sig) {
        stats->signatures_created++;
        if (sig->is_verified) {
            stats->signatures_verified++;
            if (!sig->is_valid) {
                stats->verification_failures++;
            }
        }
        sig = sig->next;
    }
    
    stats->stats_period_start = time(NULL);
    
    pthread_mutex_unlock(&stats_mutex);
    
    return stats;
}

void crypto_stats_free(CryptoStats* stats) {
    free(stats);
}