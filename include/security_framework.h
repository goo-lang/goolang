#ifndef GOO_SECURITY_FRAMEWORK_H
#define GOO_SECURITY_FRAMEWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "ccomp_shim.h"
#include "ergonomic_errors.h"

// Forward declarations
typedef struct SecurityContext SecurityContext;
typedef struct TaintTracker TaintTracker;
typedef struct CapabilityManager CapabilityManager;
typedef struct SecurityAuditor SecurityAuditor;
typedef struct CryptoManager CryptoManager;

// Taint levels for tracking untrusted data
typedef enum {
    TAINT_NONE = 0,         // Trusted data
    TAINT_USER_INPUT,       // User-provided input
    TAINT_NETWORK,          // Data from network sources
    TAINT_FILE_SYSTEM,      // Data from file system
    TAINT_EXTERNAL_API,     // Data from external APIs
    TAINT_DATABASE,         // Data from database queries
    TAINT_ENVIRONMENT,      // Environment variables
    TAINT_COMMAND_LINE,     // Command line arguments
    TAINT_CRYPTO_WEAK,      // Weakly secured cryptographic data
    TAINT_UNVALIDATED,      // Unvalidated input data
    TAINT_SANITIZATION_PENDING, // Data awaiting sanitization
    TAINT_HIGH_RISK         // High-risk data requiring special handling
} TaintLevel;

// Security capabilities for fine-grained permissions
typedef enum {
    CAP_NONE = 0,
    CAP_READ_FILE = 1 << 0,
    CAP_WRITE_FILE = 1 << 1,
    CAP_EXECUTE_FILE = 1 << 2,
    CAP_NETWORK_CONNECT = 1 << 3,
    CAP_NETWORK_BIND = 1 << 4,
    CAP_NETWORK_LISTEN = 1 << 5,
    CAP_PROCESS_SPAWN = 1 << 6,
    CAP_PROCESS_KILL = 1 << 7,
    CAP_MEMORY_ALLOC = 1 << 8,
    CAP_MEMORY_EXEC = 1 << 9,
    CAP_CRYPTO_ENCRYPT = 1 << 10,
    CAP_CRYPTO_DECRYPT = 1 << 11,
    CAP_CRYPTO_SIGN = 1 << 12,
    CAP_CRYPTO_VERIFY = 1 << 13,
    CAP_DATABASE_READ = 1 << 14,
    CAP_DATABASE_WRITE = 1 << 15,
    CAP_SYSTEM_CONFIG = 1 << 16,
    CAP_USER_ADMIN = 1 << 17,
    CAP_TIME_MODIFY = 1 << 18,
    CAP_HARDWARE_ACCESS = 1 << 19,
    CAP_KERNEL_MODULE = 1 << 20,
    CAP_DEBUG_TRACE = 1 << 21,
    CAP_SANDBOX_ESCAPE = 1 << 22,
    CAP_PRIVILEGE_ESCALATE = 1 << 23
} SecurityCapability;

// Security policies
typedef enum {
    SECURITY_POLICY_STRICT,      // Deny by default, explicit allow
    SECURITY_POLICY_MODERATE,    // Common operations allowed, dangerous ones restricted
    SECURITY_POLICY_PERMISSIVE,  // Most operations allowed, only dangerous ones restricted
    SECURITY_POLICY_DEVELOPMENT, // Development mode with additional debugging capabilities
    SECURITY_POLICY_AUDIT_ONLY   // Log all operations but don't enforce restrictions
} SecurityPolicy;

// Cryptographic algorithms and strengths
typedef enum {
    CRYPTO_NONE = 0,
    CRYPTO_AES_128,
    CRYPTO_AES_256,
    CRYPTO_CHACHA20_POLY1305,
    CRYPTO_RSA_2048,
    CRYPTO_RSA_4096,
    CRYPTO_ECC_P256,
    CRYPTO_ECC_P384,
    CRYPTO_ECC_P521,
    CRYPTO_ED25519,
    CRYPTO_X25519,
    CRYPTO_SHA256,
    CRYPTO_SHA384,
    CRYPTO_SHA512,
    CRYPTO_BLAKE3,
    CRYPTO_ARGON2ID,
    CRYPTO_SCRYPT,
    CRYPTO_PBKDF2_SHA256
} CryptoAlgorithm;

// Security annotations for compiler integration
typedef struct SecurityAnnotation {
    char name[64];
    TaintLevel taint_level;
    uint32_t required_capabilities;
    bool requires_sanitization;
    bool allows_serialization;
    bool requires_encryption;
    const char* validation_function;
    const char* sanitization_function;
    const char* audit_category;
} SecurityAnnotation;

// Tainted data wrapper
typedef struct TaintedData {
    void* data;
    size_t size;
    TaintLevel taint_level;
    uint64_t source_id;
    uint64_t creation_time;
    char source_description[128];
    
    // Propagation tracking
    struct TaintedData** sources;
    size_t source_count;
    size_t source_capacity;
    
    // Sanitization status
    bool is_sanitized;
    char sanitization_method[64];
    uint64_t sanitization_time;
    
    // Validation status
    bool is_validated;
    char validation_method[64];
    uint64_t validation_time;
    
    struct TaintedData* next;
} TaintedData;

// Taint tracker for propagation analysis
typedef struct TaintTracker {
    // Tainted data registry
    TaintedData** tainted_objects;
    size_t object_count;
    size_t object_capacity;
    
    // Taint propagation rules
    struct {
        TaintLevel input_level;
        TaintLevel output_level;
        const char* operation_type;
        bool (*propagation_rule)(TaintLevel input, const char* operation);
    } *propagation_rules;
    size_t rule_count;
    size_t rule_capacity;
    
    // Statistics
    uint64_t total_taint_operations;
    uint64_t taint_propagations;
    uint64_t sanitization_operations;
    uint64_t validation_operations;
    uint64_t security_violations;
    
    // Configuration
    bool enable_automatic_sanitization;
    bool enable_taint_warnings;
    bool enable_strict_propagation;
    TaintLevel default_taint_level;
    
    pthread_mutex_t tracker_mutex;
} TaintTracker;

// Capability-based security manager
typedef struct CapabilitySet {
    uint32_t granted_capabilities;
    uint32_t denied_capabilities;
    uint32_t requested_capabilities;
    
    // Temporal restrictions
    uint64_t valid_from;
    uint64_t valid_until;
    uint64_t max_usage_count;
    uint64_t current_usage_count;
    
    // Contextual restrictions
    char allowed_functions[32][64];
    size_t allowed_function_count;
    char allowed_modules[16][64];
    size_t allowed_module_count;
    
    // Delegation
    bool can_delegate;
    uint32_t delegatable_capabilities;
    
    struct CapabilitySet* next;
} CapabilitySet;

typedef struct CapabilityManager {
    // System-wide capabilities
    uint32_t system_capabilities;
    uint32_t admin_capabilities;
    uint32_t user_capabilities;
    uint32_t guest_capabilities;
    
    // Active capability sets
    CapabilitySet** active_sets;
    size_t set_count;
    size_t set_capacity;
    
    // Capability inheritance rules
    struct {
        const char* parent_function;
        const char* child_function;
        uint32_t inherited_capabilities;
        bool inherit_all;
    } *inheritance_rules;
    size_t inheritance_rule_count;
    
    // Audit logging
    bool enable_capability_audit;
    char audit_log_path[256];
    
    // Statistics
    uint64_t capability_checks;
    uint64_t capability_denials;
    uint64_t capability_grants;
    uint64_t delegation_operations;
    
    pthread_mutex_t capability_mutex;
} CapabilityManager;

// Security auditor for compile-time and runtime analysis
typedef struct SecurityViolation {
    uint64_t violation_id;
    uint64_t timestamp;
    
    enum {
        VIOLATION_TAINT_LEAK,
        VIOLATION_CAPABILITY_EXCEEDED,
        VIOLATION_UNSAFE_OPERATION,
        VIOLATION_CRYPTO_WEAKNESS,
        VIOLATION_BUFFER_OVERFLOW,
        VIOLATION_INJECTION_ATTACK,
        VIOLATION_PRIVILEGE_ESCALATION,
        VIOLATION_SIDE_CHANNEL,
        VIOLATION_TIMING_ATTACK,
        VIOLATION_MEMORY_CORRUPTION,
        VIOLATION_RACE_CONDITION,
        VIOLATION_RESOURCE_EXHAUSTION
    } violation_type;
    
    enum {
        SEVERITY_INFO,
        SEVERITY_LOW,
        SEVERITY_MEDIUM,
        SEVERITY_HIGH,
        SEVERITY_CRITICAL
    } severity;
    
    char description[256];
    char location[128];
    char function_name[64];
    char module_name[64];
    
    // Context information
    TaintLevel involved_taint_level;
    uint32_t involved_capabilities;
    char attack_vector[128];
    char mitigation_suggestion[256];
    
    // Evidence
    void* evidence_data;
    size_t evidence_size;
    
    struct SecurityViolation* next;
} SecurityViolation;

typedef struct SecurityAuditor {
    // Violation tracking
    SecurityViolation* violations;
    SecurityViolation* violation_tail;
    size_t violation_count;
    
    // Audit rules
    struct {
        const char* pattern;
        bool (*check_function)(const void* context);
        enum {
            AUDIT_COMPILE_TIME,
            AUDIT_RUNTIME,
            AUDIT_BOTH
        } audit_phase;
        bool is_enabled;
    } *audit_rules;
    size_t rule_count;
    size_t rule_capacity;
    
    // Configuration
    bool enable_runtime_auditing;
    bool enable_compile_time_auditing;
    bool enable_automatic_mitigation;
    bool block_on_violation;
    
    // Statistics
    uint64_t total_audits;
    uint64_t violations_detected;
    uint64_t violations_mitigated;
    uint64_t false_positives;
    
    // Integration
    TaintTracker* taint_tracker;
    CapabilityManager* capability_manager;
    
    pthread_mutex_t auditor_mutex;
} SecurityAuditor;

// Cryptographic defaults and management
typedef struct CryptoConfig {
    // Default algorithms
    CryptoAlgorithm default_symmetric_cipher;
    CryptoAlgorithm default_asymmetric_cipher;
    CryptoAlgorithm default_hash_algorithm;
    CryptoAlgorithm default_key_derivation;
    CryptoAlgorithm default_signature_algorithm;
    
    // Security parameters
    size_t minimum_key_length;
    size_t minimum_salt_length;
    size_t minimum_iv_length;
    uint32_t minimum_iteration_count;
    
    // Validation rules
    bool reject_weak_algorithms;
    bool require_authenticated_encryption;
    bool require_forward_secrecy;
    bool require_constant_time_operations;
    
    // Key management
    bool auto_key_rotation;
    uint64_t key_rotation_interval_seconds;
    bool secure_key_deletion;
    bool hardware_key_storage_preferred;
} CryptoConfig;

typedef struct CryptoManager {
    CryptoConfig config;
    
    // Key management
    struct {
        uint64_t key_id;
        CryptoAlgorithm algorithm;
        void* key_material;
        size_t key_length;
        uint64_t creation_time;
        uint64_t expiration_time;
        uint32_t usage_count;
        uint32_t max_usage_count;
        bool is_hardware_backed;
    } *keys;
    size_t key_count;
    size_t key_capacity;
    
    // Random number generation
    bool use_hardware_rng;
    bool entropy_pool_initialized;
    
    // Statistics
    uint64_t encryption_operations;
    uint64_t decryption_operations;
    uint64_t signing_operations;
    uint64_t verification_operations;
    uint64_t key_generation_operations;
    uint64_t weak_crypto_rejections;
    
    pthread_mutex_t crypto_mutex;
} CryptoManager;

// Main security context
typedef struct SecurityContext {
    SecurityPolicy policy;
    
    // Component managers
    TaintTracker* taint_tracker;
    CapabilityManager* capability_manager;
    SecurityAuditor* auditor;
    CryptoManager* crypto_manager;
    
    // Global security state
    bool security_enabled;
    bool development_mode;
    bool audit_mode_only;
    
    // Compile-time integration
    bool enable_compile_time_checks;
    bool enable_automatic_annotations;
    bool enable_security_optimizations;
    
    // Runtime integration
    bool enable_runtime_monitoring;
    bool enable_dynamic_taint_tracking;
    bool enable_capability_enforcement;
    
    // Statistics
    uint64_t total_security_checks;
    uint64_t security_violations_blocked;
    uint64_t security_warnings_issued;
    
    pthread_mutex_t context_mutex;
} SecurityContext;

// Core security framework operations
SecurityContext* security_context_create(SecurityPolicy policy);
void security_context_destroy(SecurityContext* context);
Result_void_ptr security_context_initialize(SecurityContext* context);
Result_void_ptr security_context_shutdown(SecurityContext* context);

// Taint tracking operations
TaintTracker* taint_tracker_create(void);
void taint_tracker_destroy(TaintTracker* tracker);
TaintedData* taint_data_create(void* data, size_t size, TaintLevel taint_level, const char* source);
void taint_data_destroy(TaintedData* tainted);
Result_void_ptr taint_propagate(TaintTracker* tracker, TaintedData* input, TaintedData* output, const char* operation);
Result_void_ptr taint_sanitize(TaintTracker* tracker, TaintedData* tainted, const char* sanitization_method);
Result_void_ptr taint_validate(TaintTracker* tracker, TaintedData* tainted, const char* validation_method);
bool taint_is_safe_for_operation(TaintedData* tainted, const char* operation);

// Capability management operations
CapabilityManager* capability_manager_create(void);
void capability_manager_destroy(CapabilityManager* manager);
CapabilitySet* capability_set_create(uint32_t capabilities);
void capability_set_destroy(CapabilitySet* set);
Result_bool capability_check(CapabilityManager* manager, uint32_t required_capabilities, const char* operation);
Result_void_ptr capability_grant(CapabilityManager* manager, uint32_t capabilities, const char* context);
Result_void_ptr capability_revoke(CapabilityManager* manager, uint32_t capabilities, const char* context);
Result_void_ptr capability_delegate(CapabilityManager* manager, uint32_t capabilities, const char* target);

// Security auditing operations
SecurityAuditor* security_auditor_create(void);
void security_auditor_destroy(SecurityAuditor* auditor);
Result_void_ptr security_audit_operation(SecurityAuditor* auditor, const char* operation, const void* context);
Result_void_ptr security_audit_compile_time(SecurityAuditor* auditor, const char* code, const char* location);
SecurityViolation* security_violation_create(int violation_type, int severity, const char* description, const char* location);
void security_violation_destroy(SecurityViolation* violation);
Result_void_ptr security_audit_report_violation(SecurityAuditor* auditor, SecurityViolation* violation);

// Cryptographic operations
CryptoManager* crypto_manager_create(void);
void crypto_manager_destroy(CryptoManager* manager);
Result_void_ptr crypto_encrypt(CryptoManager* manager, const void* plaintext, size_t plaintext_len, void** ciphertext, size_t* ciphertext_len);
Result_void_ptr crypto_decrypt(CryptoManager* manager, const void* ciphertext, size_t ciphertext_len, void** plaintext, size_t* plaintext_len);
Result_void_ptr crypto_sign(CryptoManager* manager, const void* data, size_t data_len, void** signature, size_t* signature_len);
Result_bool crypto_verify(CryptoManager* manager, const void* data, size_t data_len, const void* signature, size_t signature_len);
Result_void_ptr crypto_generate_key(CryptoManager* manager, CryptoAlgorithm algorithm, uint64_t* key_id);
Result_void_ptr crypto_derive_key(CryptoManager* manager, const char* password, const void* salt, size_t salt_len, uint64_t* key_id);

// Compiler integration macros
#define TAINTED(level) __attribute__((annotate("tainted:" #level)))
#define CAPABILITY(caps) __attribute__((annotate("capability:" #caps)))
#define SANITIZED_BY(func) __attribute__((annotate("sanitized_by:" #func)))
#define VALIDATED_BY(func) __attribute__((annotate("validated_by:" #func)))
#define REQUIRES_ENCRYPTION __attribute__((annotate("requires_encryption")))
#define SECURITY_CRITICAL __attribute__((annotate("security_critical")))
#define AUDIT_LOG(category) __attribute__((annotate("audit_log:" #category)))

// Security policy configuration helpers
SecurityContext* security_context_create_strict(void);
SecurityContext* security_context_create_moderate(void);
SecurityContext* security_context_create_development(void);

// Integration with existing error handling
Result_void_ptr security_integrate_error_handling(SecurityContext* context, ErgoErrorContext* error_context);

// Utility functions
const char* taint_level_to_string(TaintLevel level);
const char* capability_to_string(SecurityCapability capability);
const char* security_policy_to_string(SecurityPolicy policy);
const char* crypto_algorithm_to_string(CryptoAlgorithm algorithm);

// Statistics and monitoring
typedef struct SecurityStats {
    uint64_t total_taint_operations;
    uint64_t taint_propagations;
    uint64_t sanitization_operations;
    uint64_t capability_checks;
    uint64_t capability_violations;
    uint64_t security_violations;
    uint64_t crypto_operations;
    uint64_t audit_events;
    
    // Performance metrics
    double avg_taint_check_time_ns;
    double avg_capability_check_time_ns;
    double avg_audit_time_ns;
    
    // Security effectiveness
    double violation_detection_rate;
    double false_positive_rate;
    uint64_t attacks_prevented;
} SecurityStats;

SecurityStats security_get_stats(SecurityContext* context);
void security_reset_stats(SecurityContext* context);

// Error codes specific to security framework
#define ERROR_SECURITY_VIOLATION        0x5001
#define ERROR_TAINT_PROPAGATION         0x5002
#define ERROR_CAPABILITY_DENIED         0x5003
#define ERROR_SANITIZATION_FAILED       0x5004
#define ERROR_VALIDATION_FAILED         0x5005
#define ERROR_CRYPTO_OPERATION_FAILED   0x5006
#define ERROR_AUDIT_FAILURE             0x5007
#define ERROR_SECURITY_POLICY_VIOLATION 0x5008

#endif // GOO_SECURITY_FRAMEWORK_H