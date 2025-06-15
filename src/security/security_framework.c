#include "../../include/security_framework.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

// Utility functions
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t generate_unique_id(void) {
    static _Atomic uint64_t counter = 1;
    return atomic_fetch_add(&counter, 1);
}

// String conversion utilities
const char* taint_level_to_string(TaintLevel level) {
    switch (level) {
        case TAINT_NONE: return "none";
        case TAINT_USER_INPUT: return "user_input";
        case TAINT_NETWORK: return "network";
        case TAINT_FILE_SYSTEM: return "file_system";
        case TAINT_EXTERNAL_API: return "external_api";
        case TAINT_DATABASE: return "database";
        case TAINT_ENVIRONMENT: return "environment";
        case TAINT_COMMAND_LINE: return "command_line";
        case TAINT_CRYPTO_WEAK: return "crypto_weak";
        case TAINT_UNVALIDATED: return "unvalidated";
        case TAINT_SANITIZATION_PENDING: return "sanitization_pending";
        case TAINT_HIGH_RISK: return "high_risk";
        default: return "unknown";
    }
}

const char* capability_to_string(SecurityCapability capability) {
    switch (capability) {
        case CAP_READ_FILE: return "read_file";
        case CAP_WRITE_FILE: return "write_file";
        case CAP_EXECUTE_FILE: return "execute_file";
        case CAP_NETWORK_CONNECT: return "network_connect";
        case CAP_NETWORK_BIND: return "network_bind";
        case CAP_NETWORK_LISTEN: return "network_listen";
        case CAP_PROCESS_SPAWN: return "process_spawn";
        case CAP_PROCESS_KILL: return "process_kill";
        case CAP_MEMORY_ALLOC: return "memory_alloc";
        case CAP_MEMORY_EXEC: return "memory_exec";
        case CAP_CRYPTO_ENCRYPT: return "crypto_encrypt";
        case CAP_CRYPTO_DECRYPT: return "crypto_decrypt";
        case CAP_CRYPTO_SIGN: return "crypto_sign";
        case CAP_CRYPTO_VERIFY: return "crypto_verify";
        case CAP_DATABASE_READ: return "database_read";
        case CAP_DATABASE_WRITE: return "database_write";
        case CAP_SYSTEM_CONFIG: return "system_config";
        case CAP_USER_ADMIN: return "user_admin";
        case CAP_TIME_MODIFY: return "time_modify";
        case CAP_HARDWARE_ACCESS: return "hardware_access";
        case CAP_KERNEL_MODULE: return "kernel_module";
        case CAP_DEBUG_TRACE: return "debug_trace";
        case CAP_SANDBOX_ESCAPE: return "sandbox_escape";
        case CAP_PRIVILEGE_ESCALATE: return "privilege_escalate";
        default: return "unknown";
    }
}

const char* security_policy_to_string(SecurityPolicy policy) {
    switch (policy) {
        case SECURITY_POLICY_STRICT: return "strict";
        case SECURITY_POLICY_MODERATE: return "moderate";
        case SECURITY_POLICY_PERMISSIVE: return "permissive";
        case SECURITY_POLICY_DEVELOPMENT: return "development";
        case SECURITY_POLICY_AUDIT_ONLY: return "audit_only";
        default: return "unknown";
    }
}

const char* crypto_algorithm_to_string(CryptoAlgorithm algorithm) {
    switch (algorithm) {
        case CRYPTO_AES_128: return "aes_128";
        case CRYPTO_AES_256: return "aes_256";
        case CRYPTO_CHACHA20_POLY1305: return "chacha20_poly1305";
        case CRYPTO_RSA_2048: return "rsa_2048";
        case CRYPTO_RSA_4096: return "rsa_4096";
        case CRYPTO_ECC_P256: return "ecc_p256";
        case CRYPTO_ECC_P384: return "ecc_p384";
        case CRYPTO_ECC_P521: return "ecc_p521";
        case CRYPTO_ED25519: return "ed25519";
        case CRYPTO_X25519: return "x25519";
        case CRYPTO_SHA256: return "sha256";
        case CRYPTO_SHA384: return "sha384";
        case CRYPTO_SHA512: return "sha512";
        case CRYPTO_BLAKE3: return "blake3";
        case CRYPTO_ARGON2ID: return "argon2id";
        case CRYPTO_SCRYPT: return "scrypt";
        case CRYPTO_PBKDF2_SHA256: return "pbkdf2_sha256";
        default: return "unknown";
    }
}

// Taint tracking implementation
TaintTracker* taint_tracker_create(void) {
    TaintTracker* tracker = calloc(1, sizeof(TaintTracker));
    if (!tracker) return NULL;
    
    tracker->object_capacity = 10000;
    tracker->tainted_objects = calloc(tracker->object_capacity, sizeof(TaintedData*));
    if (!tracker->tainted_objects) {
        free(tracker);
        return NULL;
    }
    
    tracker->rule_capacity = 100;
    tracker->propagation_rules = calloc(tracker->rule_capacity, sizeof(*tracker->propagation_rules));
    if (!tracker->propagation_rules) {
        free(tracker->tainted_objects);
        free(tracker);
        return NULL;
    }
    
    // Initialize default configuration
    tracker->enable_automatic_sanitization = false;
    tracker->enable_taint_warnings = true;
    tracker->enable_strict_propagation = true;
    tracker->default_taint_level = TAINT_USER_INPUT;
    
    if (pthread_mutex_init(&tracker->tracker_mutex, NULL) != 0) {
        free(tracker->propagation_rules);
        free(tracker->tainted_objects);
        free(tracker);
        return NULL;
    }
    
    return tracker;
}

void taint_tracker_destroy(TaintTracker* tracker) {
    if (!tracker) return;
    
    // Clean up tainted objects
    if (tracker->tainted_objects) {
        for (size_t i = 0; i < tracker->object_count; i++) {
            if (tracker->tainted_objects[i]) {
                taint_data_destroy(tracker->tainted_objects[i]);
            }
        }
        free(tracker->tainted_objects);
    }
    
    free(tracker->propagation_rules);
    pthread_mutex_destroy(&tracker->tracker_mutex);
    free(tracker);
}

TaintedData* taint_data_create(void* data, size_t size, TaintLevel taint_level, const char* source) {
    TaintedData* tainted = calloc(1, sizeof(TaintedData));
    if (!tainted) return NULL;
    
    tainted->data = data;
    tainted->size = size;
    tainted->taint_level = taint_level;
    tainted->source_id = generate_unique_id();
    tainted->creation_time = get_current_time_ns();
    
    if (source) {
        strncpy(tainted->source_description, source, sizeof(tainted->source_description) - 1);
        tainted->source_description[sizeof(tainted->source_description) - 1] = '\0';
    }
    
    // Initialize source tracking
    tainted->source_capacity = 10;
    tainted->sources = calloc(tainted->source_capacity, sizeof(TaintedData*));
    if (!tainted->sources) {
        free(tainted);
        return NULL;
    }
    
    tainted->is_sanitized = false;
    tainted->is_validated = false;
    
    return tainted;
}

void taint_data_destroy(TaintedData* tainted) {
    if (!tainted) return;
    
    // Clean up source references
    if (tainted->sources) {
        // Note: We don't destroy the source objects themselves,
        // just the references to them
        free(tainted->sources);
    }
    
    free(tainted);
}

Result_void_ptr taint_propagate(TaintTracker* tracker, TaintedData* input, TaintedData* output, const char* operation) {
    if (!tracker || !input || !output || !operation) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters for taint propagation"),
            .hint = strdup("Ensure all parameters are non-null"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&tracker->tracker_mutex);
    
    // Default propagation: output inherits the highest taint level
    TaintLevel new_taint_level = input->taint_level;
    
    // Check propagation rules
    for (size_t i = 0; i < tracker->rule_count; i++) {
        if (tracker->propagation_rules[i].input_level == input->taint_level &&
            strcmp(tracker->propagation_rules[i].operation_type, operation) == 0) {
            
            if (tracker->propagation_rules[i].propagation_rule) {
                if (tracker->propagation_rules[i].propagation_rule(input->taint_level, operation)) {
                    new_taint_level = tracker->propagation_rules[i].output_level;
                }
            } else {
                new_taint_level = tracker->propagation_rules[i].output_level;
            }
            break;
        }
    }
    
    // Update output taint level
    if (new_taint_level > output->taint_level) {
        output->taint_level = new_taint_level;
    }
    
    // Add input as a source for the output
    if (output->source_count < output->source_capacity) {
        output->sources[output->source_count++] = input;
    }
    
    // Update statistics
    tracker->taint_propagations++;
    tracker->total_taint_operations++;
    
    pthread_mutex_unlock(&tracker->tracker_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr taint_sanitize(TaintTracker* tracker, TaintedData* tainted, const char* sanitization_method) {
    if (!tracker || !tainted || !sanitization_method) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters for taint sanitization"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&tracker->tracker_mutex);
    
    // Mark as sanitized
    tainted->is_sanitized = true;
    tainted->sanitization_time = get_current_time_ns();
    strncpy(tainted->sanitization_method, sanitization_method, sizeof(tainted->sanitization_method) - 1);
    tainted->sanitization_method[sizeof(tainted->sanitization_method) - 1] = '\0';
    
    // Reduce taint level after sanitization
    switch (tainted->taint_level) {
        case TAINT_HIGH_RISK:
            tainted->taint_level = TAINT_USER_INPUT;
            break;
        case TAINT_UNVALIDATED:
            tainted->taint_level = TAINT_USER_INPUT;
            break;
        case TAINT_SANITIZATION_PENDING:
            tainted->taint_level = TAINT_NONE;
            break;
        case TAINT_USER_INPUT:
        case TAINT_NETWORK:
        case TAINT_FILE_SYSTEM:
        case TAINT_EXTERNAL_API:
        case TAINT_DATABASE:
            tainted->taint_level = TAINT_NONE;
            break;
        default:
            // Other taint levels may not be fully sanitizable
            break;
    }
    
    tracker->sanitization_operations++;
    tracker->total_taint_operations++;
    
    pthread_mutex_unlock(&tracker->tracker_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr taint_validate(TaintTracker* tracker, TaintedData* tainted, const char* validation_method) {
    if (!tracker || !tainted || !validation_method) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters for taint validation"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&tracker->tracker_mutex);
    
    // Mark as validated
    tainted->is_validated = true;
    tainted->validation_time = get_current_time_ns();
    strncpy(tainted->validation_method, validation_method, sizeof(tainted->validation_method) - 1);
    tainted->validation_method[sizeof(tainted->validation_method) - 1] = '\0';
    
    tracker->validation_operations++;
    tracker->total_taint_operations++;
    
    pthread_mutex_unlock(&tracker->tracker_mutex);
    
    return OK_PTR(NULL);
}

bool taint_is_safe_for_operation(TaintedData* tainted, const char* operation) {
    if (!tainted || !operation) return false;
    
    // High-risk tainted data is never safe for sensitive operations
    if (tainted->taint_level == TAINT_HIGH_RISK) {
        return false;
    }
    
    // Check if operation requires sanitized or validated data
    if (strstr(operation, "sql") || strstr(operation, "database") || strstr(operation, "query")) {
        return tainted->is_sanitized && tainted->is_validated;
    }
    
    if (strstr(operation, "exec") || strstr(operation, "command") || strstr(operation, "shell")) {
        return tainted->is_sanitized && tainted->is_validated;
    }
    
    if (strstr(operation, "file") || strstr(operation, "path")) {
        return tainted->is_validated;
    }
    
    // For other operations, sanitized data is generally safe
    return tainted->is_sanitized || tainted->taint_level == TAINT_NONE;
}

// Capability management implementation
CapabilityManager* capability_manager_create(void) {
    CapabilityManager* manager = calloc(1, sizeof(CapabilityManager));
    if (!manager) return NULL;
    
    // Set default system capabilities
    manager->system_capabilities = CAP_READ_FILE | CAP_WRITE_FILE | CAP_NETWORK_CONNECT | 
                                  CAP_PROCESS_SPAWN | CAP_MEMORY_ALLOC | CAP_CRYPTO_ENCRYPT | 
                                  CAP_CRYPTO_DECRYPT | CAP_DATABASE_READ | CAP_DATABASE_WRITE;
    
    manager->admin_capabilities = manager->system_capabilities | CAP_EXECUTE_FILE | 
                                 CAP_NETWORK_BIND | CAP_NETWORK_LISTEN | CAP_PROCESS_KILL |
                                 CAP_MEMORY_EXEC | CAP_SYSTEM_CONFIG | CAP_USER_ADMIN |
                                 CAP_TIME_MODIFY | CAP_DEBUG_TRACE;
    
    manager->user_capabilities = CAP_READ_FILE | CAP_WRITE_FILE | CAP_NETWORK_CONNECT |
                                CAP_MEMORY_ALLOC | CAP_CRYPTO_ENCRYPT | CAP_CRYPTO_DECRYPT;
    
    manager->guest_capabilities = CAP_READ_FILE | CAP_MEMORY_ALLOC;
    
    manager->set_capacity = 1000;
    manager->active_sets = calloc(manager->set_capacity, sizeof(CapabilitySet*));
    if (!manager->active_sets) {
        free(manager);
        return NULL;
    }
    
    manager->inheritance_rule_count = 0;
    manager->enable_capability_audit = true;
    strncpy(manager->audit_log_path, "/var/log/goo_security.log", sizeof(manager->audit_log_path) - 1);
    
    if (pthread_mutex_init(&manager->capability_mutex, NULL) != 0) {
        free(manager->active_sets);
        free(manager);
        return NULL;
    }
    
    return manager;
}

void capability_manager_destroy(CapabilityManager* manager) {
    if (!manager) return;
    
    // Clean up active capability sets
    if (manager->active_sets) {
        for (size_t i = 0; i < manager->set_count; i++) {
            if (manager->active_sets[i]) {
                capability_set_destroy(manager->active_sets[i]);
            }
        }
        free(manager->active_sets);
    }
    
    free(manager->inheritance_rules);
    pthread_mutex_destroy(&manager->capability_mutex);
    free(manager);
}

CapabilitySet* capability_set_create(uint32_t capabilities) {
    CapabilitySet* set = calloc(1, sizeof(CapabilitySet));
    if (!set) return NULL;
    
    set->granted_capabilities = capabilities;
    set->denied_capabilities = 0;
    set->requested_capabilities = 0;
    
    set->valid_from = get_current_time_ns();
    set->valid_until = UINT64_MAX;  // Never expires by default
    set->max_usage_count = UINT64_MAX;  // Unlimited usage by default
    set->current_usage_count = 0;
    
    set->can_delegate = false;
    set->delegatable_capabilities = 0;
    
    return set;
}

void capability_set_destroy(CapabilitySet* set) {
    if (!set) return;
    free(set);
}

Result_bool capability_check(CapabilityManager* manager, uint32_t required_capabilities, const char* operation) {
    if (!manager) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid capability manager"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_bool){.is_error = true, .error = error};
    }
    
    pthread_mutex_lock(&manager->capability_mutex);
    
    manager->capability_checks++;
    
    // Check against current active capability sets
    bool has_capability = false;
    uint64_t current_time = get_current_time_ns();
    
    for (size_t i = 0; i < manager->set_count; i++) {
        CapabilitySet* set = manager->active_sets[i];
        if (!set) continue;
        
        // Check temporal validity
        if (current_time < set->valid_from || current_time > set->valid_until) {
            continue;
        }
        
        // Check usage count
        if (set->current_usage_count >= set->max_usage_count) {
            continue;
        }
        
        // Check if the set has the required capabilities
        if ((set->granted_capabilities & required_capabilities) == required_capabilities) {
            // Check if any required capabilities are explicitly denied
            if ((set->denied_capabilities & required_capabilities) == 0) {
                has_capability = true;
                set->current_usage_count++;
                break;
            }
        }
    }
    
    if (has_capability) {
        manager->capability_grants++;
    } else {
        manager->capability_denials++;
        
        // Log capability denial if auditing is enabled
        if (manager->enable_capability_audit) {
            // TODO: Implement audit logging
        }
    }
    
    pthread_mutex_unlock(&manager->capability_mutex);
    
    return (Result_bool){.is_error = false, .value = has_capability};
}

Result_void_ptr capability_grant(CapabilityManager* manager, uint32_t capabilities, const char* context) {
    if (!manager) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid capability manager"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&manager->capability_mutex);
    
    // Create new capability set
    CapabilitySet* new_set = capability_set_create(capabilities);
    if (!new_set) {
        pthread_mutex_unlock(&manager->capability_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to create capability set"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Add to active sets
    if (manager->set_count >= manager->set_capacity) {
        capability_set_destroy(new_set);
        pthread_mutex_unlock(&manager->capability_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Capability manager at capacity"),
            .hint = strdup("Consider cleaning up unused capability sets"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    manager->active_sets[manager->set_count++] = new_set;
    manager->capability_grants++;
    
    pthread_mutex_unlock(&manager->capability_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr capability_revoke(CapabilityManager* manager, uint32_t capabilities, const char* context) {
    if (!manager) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid capability manager"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&manager->capability_mutex);
    
    // Revoke capabilities from all active sets
    for (size_t i = 0; i < manager->set_count; i++) {
        CapabilitySet* set = manager->active_sets[i];
        if (set) {
            set->granted_capabilities &= ~capabilities;
            set->denied_capabilities |= capabilities;
        }
    }
    
    pthread_mutex_unlock(&manager->capability_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr capability_delegate(CapabilityManager* manager, uint32_t capabilities, const char* target) {
    if (!manager || !target) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters for capability delegation"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&manager->capability_mutex);
    
    // Check if delegation is possible
    bool can_delegate = false;
    for (size_t i = 0; i < manager->set_count; i++) {
        CapabilitySet* set = manager->active_sets[i];
        if (set && set->can_delegate) {
            if ((set->delegatable_capabilities & capabilities) == capabilities) {
                can_delegate = true;
                break;
            }
        }
    }
    
    if (!can_delegate) {
        pthread_mutex_unlock(&manager->capability_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_CAPABILITY_DENIED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Capability delegation not permitted"),
            .hint = strdup("Ensure delegator has delegation rights for these capabilities"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Create delegated capability set
    CapabilitySet* delegated_set = capability_set_create(capabilities);
    if (!delegated_set) {
        pthread_mutex_unlock(&manager->capability_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to create delegated capability set"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Add to active sets if there's space
    if (manager->set_count < manager->set_capacity) {
        manager->active_sets[manager->set_count++] = delegated_set;
        manager->delegation_operations++;
    } else {
        capability_set_destroy(delegated_set);
        pthread_mutex_unlock(&manager->capability_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Cannot delegate: capability manager at capacity"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_unlock(&manager->capability_mutex);
    
    return OK_PTR(NULL);
}

// Security auditor implementation
SecurityAuditor* security_auditor_create(void) {
    SecurityAuditor* auditor = calloc(1, sizeof(SecurityAuditor));
    if (!auditor) return NULL;
    
    auditor->rule_capacity = 200;
    auditor->audit_rules = calloc(auditor->rule_capacity, sizeof(*auditor->audit_rules));
    if (!auditor->audit_rules) {
        free(auditor);
        return NULL;
    }
    
    // Default configuration
    auditor->enable_runtime_auditing = true;
    auditor->enable_compile_time_auditing = true;
    auditor->enable_automatic_mitigation = false;
    auditor->block_on_violation = true;
    
    if (pthread_mutex_init(&auditor->auditor_mutex, NULL) != 0) {
        free(auditor->audit_rules);
        free(auditor);
        return NULL;
    }
    
    return auditor;
}

void security_auditor_destroy(SecurityAuditor* auditor) {
    if (!auditor) return;
    
    // Clean up violations
    SecurityViolation* violation = auditor->violations;
    while (violation) {
        SecurityViolation* next = violation->next;
        security_violation_destroy(violation);
        violation = next;
    }
    
    free(auditor->audit_rules);
    pthread_mutex_destroy(&auditor->auditor_mutex);
    free(auditor);
}

SecurityViolation* security_violation_create(int violation_type, int severity, const char* description, const char* location) {
    SecurityViolation* violation = calloc(1, sizeof(SecurityViolation));
    if (!violation) return NULL;
    
    violation->violation_id = generate_unique_id();
    violation->timestamp = get_current_time_ns();
    violation->violation_type = violation_type;
    violation->severity = severity;
    
    if (description) {
        strncpy(violation->description, description, sizeof(violation->description) - 1);
        violation->description[sizeof(violation->description) - 1] = '\0';
    }
    
    if (location) {
        strncpy(violation->location, location, sizeof(violation->location) - 1);
        violation->location[sizeof(violation->location) - 1] = '\0';
    }
    
    return violation;
}

void security_violation_destroy(SecurityViolation* violation) {
    if (!violation) return;
    
    if (violation->evidence_data) {
        free(violation->evidence_data);
    }
    
    free(violation);
}

Result_void_ptr security_audit_report_violation(SecurityAuditor* auditor, SecurityViolation* violation) {
    if (!auditor || !violation) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid auditor or violation"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&auditor->auditor_mutex);
    
    // Add to violations list
    if (!auditor->violations) {
        auditor->violations = violation;
        auditor->violation_tail = violation;
    } else {
        auditor->violation_tail->next = violation;
        auditor->violation_tail = violation;
    }
    auditor->violation_count++;
    auditor->violations_detected++;
    
    pthread_mutex_unlock(&auditor->auditor_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr security_audit_operation(SecurityAuditor* auditor, const char* operation, const void* context) {
    if (!auditor || !operation) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters for security audit"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&auditor->auditor_mutex);
    
    auditor->total_audits++;
    
    // Run applicable audit rules
    for (size_t i = 0; i < auditor->rule_count; i++) {
        if (auditor->audit_rules[i].is_enabled &&
            (auditor->audit_rules[i].audit_phase == AUDIT_RUNTIME || 
             auditor->audit_rules[i].audit_phase == AUDIT_BOTH)) {
            
            if (strstr(operation, auditor->audit_rules[i].pattern)) {
                if (auditor->audit_rules[i].check_function) {
                    if (!auditor->audit_rules[i].check_function(context)) {
                        // Create violation
                        SecurityViolation* violation = security_violation_create(
                            VIOLATION_UNSAFE_OPERATION,
                            SEVERITY_MEDIUM,
                            "Security audit rule violation",
                            operation
                        );
                        
                        if (violation) {
                            security_audit_report_violation(auditor, violation);
                        }
                    }
                }
            }
        }
    }
    
    pthread_mutex_unlock(&auditor->auditor_mutex);
    
    return OK_PTR(NULL);
}

// Crypto manager implementation
CryptoManager* crypto_manager_create(void) {
    CryptoManager* manager = calloc(1, sizeof(CryptoManager));
    if (!manager) return NULL;
    
    // Set secure defaults
    manager->config.default_symmetric_cipher = CRYPTO_AES_256;
    manager->config.default_asymmetric_cipher = CRYPTO_ECC_P384;
    manager->config.default_hash_algorithm = CRYPTO_SHA256;
    manager->config.default_key_derivation = CRYPTO_ARGON2ID;
    manager->config.default_signature_algorithm = CRYPTO_ED25519;
    
    manager->config.minimum_key_length = 256;
    manager->config.minimum_salt_length = 32;
    manager->config.minimum_iv_length = 16;
    manager->config.minimum_iteration_count = 100000;
    
    manager->config.reject_weak_algorithms = true;
    manager->config.require_authenticated_encryption = true;
    manager->config.require_forward_secrecy = true;
    manager->config.require_constant_time_operations = true;
    
    manager->config.auto_key_rotation = false;
    manager->config.key_rotation_interval_seconds = 86400 * 30; // 30 days
    manager->config.secure_key_deletion = true;
    manager->config.hardware_key_storage_preferred = true;
    
    manager->key_capacity = 10000;
    manager->keys = calloc(manager->key_capacity, sizeof(*manager->keys));
    if (!manager->keys) {
        free(manager);
        return NULL;
    }
    
    manager->use_hardware_rng = true;
    manager->entropy_pool_initialized = false;
    
    if (pthread_mutex_init(&manager->crypto_mutex, NULL) != 0) {
        free(manager->keys);
        free(manager);
        return NULL;
    }
    
    return manager;
}

void crypto_manager_destroy(CryptoManager* manager) {
    if (!manager) return;
    
    // Securely destroy all keys
    if (manager->keys) {
        for (size_t i = 0; i < manager->key_count; i++) {
            if (manager->keys[i].key_material) {
                // Secure memory wiping
                memset(manager->keys[i].key_material, 0, manager->keys[i].key_length);
                free(manager->keys[i].key_material);
            }
        }
        free(manager->keys);
    }
    
    pthread_mutex_destroy(&manager->crypto_mutex);
    free(manager);
}

// Simplified crypto operations (in real implementation, use proper crypto library)
Result_void_ptr crypto_encrypt(CryptoManager* manager, const void* plaintext, size_t plaintext_len, void** ciphertext, size_t* ciphertext_len) {
    if (!manager || !plaintext || !ciphertext || !ciphertext_len) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters for encryption"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&manager->crypto_mutex);
    
    // Placeholder implementation - in real implementation, use proper crypto
    *ciphertext_len = plaintext_len + 32; // Add space for IV and authentication tag
    *ciphertext = malloc(*ciphertext_len);
    
    if (!*ciphertext) {
        pthread_mutex_unlock(&manager->crypto_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate ciphertext buffer"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // TODO: Implement actual encryption using the configured algorithm
    memcpy(*ciphertext, plaintext, plaintext_len);
    
    manager->encryption_operations++;
    
    pthread_mutex_unlock(&manager->crypto_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr crypto_decrypt(CryptoManager* manager, const void* ciphertext, size_t ciphertext_len, void** plaintext, size_t* plaintext_len) {
    if (!manager || !ciphertext || !plaintext || !plaintext_len) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters for decryption"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&manager->crypto_mutex);
    
    // Placeholder implementation
    *plaintext_len = ciphertext_len - 32; // Remove IV and tag space
    if (*plaintext_len > ciphertext_len) {
        pthread_mutex_unlock(&manager->crypto_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_CRYPTO_OPERATION_FAILED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid ciphertext length"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    *plaintext = malloc(*plaintext_len);
    if (!*plaintext) {
        pthread_mutex_unlock(&manager->crypto_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate plaintext buffer"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // TODO: Implement actual decryption
    memcpy(*plaintext, ciphertext, *plaintext_len);
    
    manager->decryption_operations++;
    
    pthread_mutex_unlock(&manager->crypto_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr crypto_generate_key(CryptoManager* manager, CryptoAlgorithm algorithm, uint64_t* key_id) {
    if (!manager || !key_id) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters for key generation"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&manager->crypto_mutex);
    
    if (manager->key_count >= manager->key_capacity) {
        pthread_mutex_unlock(&manager->crypto_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Key storage at capacity"),
            .hint = strdup("Consider cleaning up unused keys"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Determine key length based on algorithm
    size_t key_length;
    switch (algorithm) {
        case CRYPTO_AES_128:
            key_length = 16;
            break;
        case CRYPTO_AES_256:
            key_length = 32;
            break;
        case CRYPTO_ECC_P256:
            key_length = 32;
            break;
        case CRYPTO_ECC_P384:
            key_length = 48;
            break;
        case CRYPTO_ED25519:
            key_length = 32;
            break;
        default:
            key_length = 32; // Default to 256-bit keys
            break;
    }
    
    if (key_length < manager->config.minimum_key_length / 8) {
        if (manager->config.reject_weak_algorithms) {
            pthread_mutex_unlock(&manager->crypto_mutex);
            
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_CRYPTO_OPERATION_FAILED,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Algorithm does not meet minimum key length requirements"),
                .hint = strdup("Use a stronger algorithm or adjust security policy"),
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
    }
    
    // Generate new key
    uint64_t new_key_id = generate_unique_id();
    void* key_material = malloc(key_length);
    if (!key_material) {
        pthread_mutex_unlock(&manager->crypto_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate key material"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // TODO: Generate actual random key material using secure RNG
    for (size_t i = 0; i < key_length; i++) {
        ((uint8_t*)key_material)[i] = (uint8_t)(rand() % 256);
    }
    
    // Store key
    manager->keys[manager->key_count] = (typeof(manager->keys[0])){
        .key_id = new_key_id,
        .algorithm = algorithm,
        .key_material = key_material,
        .key_length = key_length,
        .creation_time = get_current_time_ns(),
        .expiration_time = UINT64_MAX,
        .usage_count = 0,
        .max_usage_count = UINT32_MAX,
        .is_hardware_backed = false
    };
    
    manager->key_count++;
    manager->key_generation_operations++;
    
    *key_id = new_key_id;
    
    pthread_mutex_unlock(&manager->crypto_mutex);
    
    return OK_PTR(NULL);
}

// Main security context operations
SecurityContext* security_context_create(SecurityPolicy policy) {
    SecurityContext* context = calloc(1, sizeof(SecurityContext));
    if (!context) return NULL;
    
    context->policy = policy;
    context->security_enabled = true;
    context->development_mode = (policy == SECURITY_POLICY_DEVELOPMENT);
    context->audit_mode_only = (policy == SECURITY_POLICY_AUDIT_ONLY);
    
    // Set integration flags based on policy
    switch (policy) {
        case SECURITY_POLICY_STRICT:
            context->enable_compile_time_checks = true;
            context->enable_automatic_annotations = true;
            context->enable_security_optimizations = true;
            context->enable_runtime_monitoring = true;
            context->enable_dynamic_taint_tracking = true;
            context->enable_capability_enforcement = true;
            break;
            
        case SECURITY_POLICY_MODERATE:
            context->enable_compile_time_checks = true;
            context->enable_automatic_annotations = false;
            context->enable_security_optimizations = true;
            context->enable_runtime_monitoring = true;
            context->enable_dynamic_taint_tracking = false;
            context->enable_capability_enforcement = true;
            break;
            
        case SECURITY_POLICY_PERMISSIVE:
            context->enable_compile_time_checks = false;
            context->enable_automatic_annotations = false;
            context->enable_security_optimizations = false;
            context->enable_runtime_monitoring = false;
            context->enable_dynamic_taint_tracking = false;
            context->enable_capability_enforcement = false;
            break;
            
        case SECURITY_POLICY_DEVELOPMENT:
            context->enable_compile_time_checks = true;
            context->enable_automatic_annotations = false;
            context->enable_security_optimizations = false;
            context->enable_runtime_monitoring = true;
            context->enable_dynamic_taint_tracking = true;
            context->enable_capability_enforcement = false;
            break;
            
        case SECURITY_POLICY_AUDIT_ONLY:
            context->enable_compile_time_checks = false;
            context->enable_automatic_annotations = false;
            context->enable_security_optimizations = false;
            context->enable_runtime_monitoring = true;
            context->enable_dynamic_taint_tracking = true;
            context->enable_capability_enforcement = false;
            break;
    }
    
    if (pthread_mutex_init(&context->context_mutex, NULL) != 0) {
        free(context);
        return NULL;
    }
    
    return context;
}

void security_context_destroy(SecurityContext* context) {
    if (!context) return;
    
    if (context->taint_tracker) {
        taint_tracker_destroy(context->taint_tracker);
    }
    
    if (context->capability_manager) {
        capability_manager_destroy(context->capability_manager);
    }
    
    if (context->auditor) {
        security_auditor_destroy(context->auditor);
    }
    
    if (context->crypto_manager) {
        crypto_manager_destroy(context->crypto_manager);
    }
    
    pthread_mutex_destroy(&context->context_mutex);
    free(context);
}

Result_void_ptr security_context_initialize(SecurityContext* context) {
    if (!context) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid security context"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Initialize components based on policy
    if (context->enable_dynamic_taint_tracking) {
        context->taint_tracker = taint_tracker_create();
        if (!context->taint_tracker) {
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_OUT_OF_MEMORY,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Failed to create taint tracker"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
    }
    
    if (context->enable_capability_enforcement) {
        context->capability_manager = capability_manager_create();
        if (!context->capability_manager) {
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_OUT_OF_MEMORY,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Failed to create capability manager"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
    }
    
    if (context->enable_runtime_monitoring) {
        context->auditor = security_auditor_create();
        if (!context->auditor) {
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_OUT_OF_MEMORY,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Failed to create security auditor"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
        
        // Link components for integrated auditing
        context->auditor->taint_tracker = context->taint_tracker;
        context->auditor->capability_manager = context->capability_manager;
    }
    
    // Always create crypto manager for secure defaults
    context->crypto_manager = crypto_manager_create();
    if (!context->crypto_manager) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to create crypto manager"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    return OK_PTR(NULL);
}

// Configuration helpers
SecurityContext* security_context_create_strict(void) {
    SecurityContext* context = security_context_create(SECURITY_POLICY_STRICT);
    if (context) {
        security_context_initialize(context);
    }
    return context;
}

SecurityContext* security_context_create_moderate(void) {
    SecurityContext* context = security_context_create(SECURITY_POLICY_MODERATE);
    if (context) {
        security_context_initialize(context);
    }
    return context;
}

SecurityContext* security_context_create_development(void) {
    SecurityContext* context = security_context_create(SECURITY_POLICY_DEVELOPMENT);
    if (context) {
        security_context_initialize(context);
    }
    return context;
}

// Statistics
SecurityStats security_get_stats(SecurityContext* context) {
    SecurityStats stats = {0};
    
    if (!context) return stats;
    
    pthread_mutex_lock(&context->context_mutex);
    
    if (context->taint_tracker) {
        stats.total_taint_operations = context->taint_tracker->total_taint_operations;
        stats.taint_propagations = context->taint_tracker->taint_propagations;
        stats.sanitization_operations = context->taint_tracker->sanitization_operations;
    }
    
    if (context->capability_manager) {
        stats.capability_checks = context->capability_manager->capability_checks;
        stats.capability_violations = context->capability_manager->capability_denials;
    }
    
    if (context->auditor) {
        stats.audit_events = context->auditor->total_audits;
        stats.security_violations = context->auditor->violations_detected;
    }
    
    if (context->crypto_manager) {
        stats.crypto_operations = context->crypto_manager->encryption_operations +
                                 context->crypto_manager->decryption_operations +
                                 context->crypto_manager->signing_operations +
                                 context->crypto_manager->verification_operations;
    }
    
    stats.attacks_prevented = context->security_violations_blocked;
    
    pthread_mutex_unlock(&context->context_mutex);
    
    return stats;
}

void security_reset_stats(SecurityContext* context) {
    if (!context) return;
    
    pthread_mutex_lock(&context->context_mutex);
    
    if (context->taint_tracker) {
        context->taint_tracker->total_taint_operations = 0;
        context->taint_tracker->taint_propagations = 0;
        context->taint_tracker->sanitization_operations = 0;
        context->taint_tracker->validation_operations = 0;
        context->taint_tracker->security_violations = 0;
    }
    
    if (context->capability_manager) {
        context->capability_manager->capability_checks = 0;
        context->capability_manager->capability_denials = 0;
        context->capability_manager->capability_grants = 0;
        context->capability_manager->delegation_operations = 0;
    }
    
    if (context->auditor) {
        context->auditor->total_audits = 0;
        context->auditor->violations_detected = 0;
        context->auditor->violations_mitigated = 0;
        context->auditor->false_positives = 0;
    }
    
    if (context->crypto_manager) {
        context->crypto_manager->encryption_operations = 0;
        context->crypto_manager->decryption_operations = 0;
        context->crypto_manager->signing_operations = 0;
        context->crypto_manager->verification_operations = 0;
        context->crypto_manager->key_generation_operations = 0;
        context->crypto_manager->weak_crypto_rejections = 0;
    }
    
    context->total_security_checks = 0;
    context->security_violations_blocked = 0;
    context->security_warnings_issued = 0;
    
    pthread_mutex_unlock(&context->context_mutex);
}