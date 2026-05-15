#ifndef GOO_CAPABILITY_SECURITY_H
#define GOO_CAPABILITY_SECURITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "ccomp_shim.h"
#include "security_framework.h"
#include "ergonomic_errors.h"
#include "ast.h"
#include "types.h"

// Forward declarations
typedef struct CapabilitySystem CapabilitySystem;
typedef struct CapabilityToken CapabilityToken;
typedef struct CapabilityPolicy CapabilityPolicy;
typedef struct CapabilityChecker CapabilityChecker;

// Capability categories for grouping related permissions
typedef enum {
    CAP_CATEGORY_FILE_SYSTEM,
    CAP_CATEGORY_NETWORK,
    CAP_CATEGORY_PROCESS,
    CAP_CATEGORY_MEMORY,
    CAP_CATEGORY_CRYPTO,
    CAP_CATEGORY_DATABASE,
    CAP_CATEGORY_SYSTEM,
    CAP_CATEGORY_HARDWARE,
    CAP_CATEGORY_USER_DEFINED
} CapabilityCategory;

// Extended capability attributes
typedef struct CapabilityAttribute {
    char name[64];
    char value[256];
    bool is_required;
    bool is_inherited;
    struct CapabilityAttribute* next;
} CapabilityAttribute;

// Capability token representing a granted permission
typedef struct CapabilityToken {
    uint64_t token_id;
    SecurityCapability capability;
    CapabilityCategory category;
    
    // Token metadata
    char granted_to[128];           // Entity (function/module/actor) granted to
    char granted_by[128];           // Authority that granted it
    uint64_t grant_time;
    
    // Validity constraints
    uint64_t valid_from;
    uint64_t valid_until;
    uint64_t max_uses;
    uint64_t current_uses;
    
    // Contextual constraints
    char* allowed_contexts;         // Comma-separated list of contexts
    char* denied_contexts;
    char* required_conditions;      // Conditions that must be met
    
    // Delegation rights
    bool can_delegate;
    int max_delegation_depth;
    int current_delegation_depth;
    uint32_t delegatable_capabilities;
    
    // Revocation status
    bool is_revoked;
    uint64_t revocation_time;
    char revocation_reason[256];
    
    // Attributes
    CapabilityAttribute* attributes;
    size_t attribute_count;
    
    // Usage tracking
    uint64_t last_used;
    uint64_t usage_count;
    double average_hold_time_ms;
    
    struct CapabilityToken* next;
} CapabilityToken;

// Capability requirement specification
typedef struct CapabilityRequirement {
    SecurityCapability required_capability;
    CapabilityCategory category;
    
    // Requirement metadata
    char description[256];
    char rationale[512];
    bool is_mandatory;
    bool is_runtime_checked;
    
    // Alternative capabilities
    SecurityCapability* alternatives;
    size_t alternative_count;
    
    // Conditions
    char condition_expression[256];
    bool (*condition_checker)(void* context);
    void* condition_context;
    
    struct CapabilityRequirement* next;
} CapabilityRequirement;

// Capability policy for different security levels
typedef struct CapabilityPolicy {
    char policy_name[64];
    char description[256];
    
    // Default capabilities for different contexts
    uint32_t default_function_capabilities;
    uint32_t default_module_capabilities;
    uint32_t default_thread_capabilities;
    uint32_t default_actor_capabilities;
    
    // Capability restrictions
    uint32_t forbidden_capabilities;
    uint32_t privileged_capabilities;     // Require special authorization
    uint32_t audit_required_capabilities; // Always audit when used
    
    // Policy rules
    struct {
        char rule_name[64];
        char pattern[128];              // Pattern to match (function/module name)
        uint32_t granted_capabilities;
        uint32_t denied_capabilities;
        bool is_regex;
        bool is_enabled;
    } *rules;
    size_t rule_count;
    size_t rule_capacity;
    
    // Delegation policy
    bool allow_delegation;
    int max_delegation_depth;
    uint32_t non_delegatable_capabilities;
    
    // Temporal policies
    uint64_t default_validity_duration_ms;
    uint64_t max_validity_duration_ms;
    bool require_periodic_renewal;
    uint64_t renewal_interval_ms;
    
    struct CapabilityPolicy* next;
} CapabilityPolicy;

// Capability binding to code entities
typedef struct CapabilityBinding {
    enum {
        BINDING_FUNCTION,
        BINDING_MODULE,
        BINDING_TYPE,
        BINDING_VARIABLE,
        BINDING_FIELD
    } binding_type;
    
    char entity_name[128];
    char entity_signature[256];
    
    // Required capabilities
    CapabilityRequirement* requirements;
    size_t requirement_count;
    
    // Granted capabilities (for delegation)
    CapabilityToken* granted_tokens;
    size_t granted_count;
    
    // Binding metadata
    char source_file[256];
    size_t line_number;
    bool is_verified;
    uint64_t verification_time;
    
    struct CapabilityBinding* next;
} CapabilityBinding;

// Runtime capability context
typedef struct CapabilityContext {
    // Current capabilities
    CapabilityToken** active_tokens;
    size_t token_count;
    size_t token_capacity;
    
    // Effective capabilities (computed from tokens)
    uint32_t effective_capabilities;
    uint64_t last_computation_time;
    
    // Context hierarchy
    struct CapabilityContext* parent;
    struct CapabilityContext** children;
    size_t child_count;
    size_t child_capacity;
    
    // Context metadata
    char context_name[128];
    enum {
        CONTEXT_GLOBAL,
        CONTEXT_MODULE,
        CONTEXT_FUNCTION,
        CONTEXT_THREAD,
        CONTEXT_ACTOR,
        CONTEXT_CUSTOM
    } context_type;
    
    // Dynamic capability acquisition
    bool allow_dynamic_acquisition;
    uint32_t acquirable_capabilities;
    
    pthread_mutex_t context_mutex;
} CapabilityContext;

// Capability checker for compile-time verification
typedef struct CapabilityChecker {
    CapabilitySystem* capability_system;
    
    // Binding registry
    CapabilityBinding** bindings;
    size_t binding_count;
    size_t binding_capacity;
    
    // Analysis state
    struct {
        ASTNode* current_function;
        char current_module[128];
        CapabilityContext* current_context;
        uint32_t current_capabilities;
        
        // Inferred requirements
        CapabilityRequirement* inferred_requirements;
        size_t inferred_count;
        
        // Violations found
        struct {
            char location[256];
            SecurityCapability missing_capability;
            char operation[128];
            char suggestion[256];
        } *violations;
        size_t violation_count;
    } analysis_state;
    
    // Configuration
    struct {
        bool enable_inference;
        bool enable_static_checking;
        bool enable_runtime_checking;
        bool strict_mode;
        bool generate_runtime_checks;
        bool annotate_ast;
    } config;
    
    pthread_mutex_t checker_mutex;
} CapabilityChecker;

// Main capability system
typedef struct CapabilitySystem {
    SecurityContext* security_context;
    
    // Token management
    CapabilityToken** tokens;
    size_t token_count;
    size_t token_capacity;
    
    // Policy management
    CapabilityPolicy** policies;
    size_t policy_count;
    size_t policy_capacity;
    CapabilityPolicy* active_policy;
    
    // Context management
    CapabilityContext* global_context;
    CapabilityContext** all_contexts;
    size_t context_count;
    size_t context_capacity;
    
    // Capability checker
    CapabilityChecker* checker;
    
    // Authority management
    struct {
        char authority_name[128];
        uint32_t grantable_capabilities;
        bool can_revoke;
        bool can_delegate;
    } *authorities;
    size_t authority_count;
    
    // Audit trail
    struct {
        uint64_t event_id;
        uint64_t timestamp;
        enum {
            AUDIT_GRANT,
            AUDIT_REVOKE,
            AUDIT_USE,
            AUDIT_DELEGATE,
            AUDIT_DENY,
            AUDIT_EXPIRE,
            AUDIT_RENEW
        } event_type;
        uint64_t token_id;
        SecurityCapability capability;
        char entity[128];
        char details[256];
        bool success;
    } *audit_log;
    size_t audit_count;
    size_t audit_capacity;
    
    // Statistics
    struct {
        uint64_t total_grants;
        uint64_t total_revocations;
        uint64_t total_uses;
        uint64_t total_denials;
        uint64_t total_delegations;
        uint64_t active_tokens;
        uint64_t expired_tokens;
        
        // Performance metrics
        uint64_t avg_check_time_ns;
        uint64_t avg_grant_time_ns;
        double cache_hit_rate;
    } stats;
    
    // Configuration
    bool is_initialized;
    bool audit_enabled;
    bool strict_enforcement;
    bool allow_capability_escalation;
    
    pthread_mutex_t system_mutex;
} CapabilitySystem;

// Core capability system operations
CapabilitySystem* capability_system_create(SecurityContext* security_context);
void capability_system_destroy(CapabilitySystem* system);
Result_void_ptr capability_system_initialize(CapabilitySystem* system);
Result_void_ptr capability_system_shutdown(CapabilitySystem* system);

// Token management
CapabilityToken* capability_token_create(SecurityCapability capability, const char* granted_to);
void capability_token_destroy(CapabilityToken* token);
Result_void_ptr capability_system_grant(CapabilitySystem* system, SecurityCapability capability, 
                                const char* entity, uint64_t validity_ms);
Result_void_ptr capability_system_revoke(CapabilitySystem* system, uint64_t token_id, const char* reason);
Result_bool capability_system_check(CapabilitySystem* system, const char* entity, SecurityCapability capability);
Result_void_ptr capability_system_use(CapabilitySystem* system, uint64_t token_id);

// Delegation operations
Result_void_ptr capability_system_delegate(CapabilitySystem* system, uint64_t token_id,
                                   const char* delegate_to, uint32_t delegated_capabilities);
Result_void_ptr capability_system_accept_delegation(CapabilitySystem* system, uint64_t delegated_token_id);

// Policy management
CapabilityPolicy* capability_policy_create(const char* name);
void capability_policy_destroy(CapabilityPolicy* policy);
Result_void_ptr capability_policy_add_rule(CapabilityPolicy* policy, const char* pattern,
                                          uint32_t granted, uint32_t denied);
Result_void_ptr capability_system_set_policy(CapabilitySystem* system, CapabilityPolicy* policy);
CapabilityPolicy* capability_policy_create_strict(void);
CapabilityPolicy* capability_policy_create_moderate(void);
CapabilityPolicy* capability_policy_create_permissive(void);

// Context management
CapabilityContext* capability_context_create(const char* name, int context_type);
void capability_context_destroy(CapabilityContext* context);
Result_void_ptr capability_context_push(CapabilitySystem* system, CapabilityContext* context);
Result_void_ptr capability_context_pop(CapabilitySystem* system);
CapabilityContext* capability_context_current(CapabilitySystem* system);

// Binding and requirement management
Result_void_ptr capability_bind_function(CapabilitySystem* system, const char* function_name,
                                        CapabilityRequirement* requirements, size_t count);
Result_void_ptr capability_bind_module(CapabilitySystem* system, const char* module_name,
                                      CapabilityRequirement* requirements, size_t count);
CapabilityRequirement* capability_requirement_create(SecurityCapability capability, 
                                                    const char* description);
void capability_requirement_destroy(CapabilityRequirement* requirement);

// Compile-time checking
CapabilityChecker* capability_checker_create(CapabilitySystem* system);
void capability_checker_destroy(CapabilityChecker* checker);
Result_void_ptr capability_check_ast(CapabilityChecker* checker, ASTNode* root);
Result_void_ptr capability_check_function(CapabilityChecker* checker, ASTNode* function_node);
Result_void_ptr capability_infer_requirements(CapabilityChecker* checker, ASTNode* node);

// Runtime capability operations
Result_void_ptr capability_acquire_dynamic(CapabilitySystem* system, SecurityCapability capability,
                                          const char* justification);
Result_void_ptr capability_release_dynamic(CapabilitySystem* system, SecurityCapability capability);
Result_bool capability_test_and_use(CapabilitySystem* system, SecurityCapability capability,
                                   const char* operation);

// Audit and monitoring
Result_void_ptr capability_audit_event(CapabilitySystem* system, int event_type,
                                      uint64_t token_id, const char* details);
Result_void_ptr capability_export_audit_log(CapabilitySystem* system, const char* filename);
typedef struct CapabilityAuditReport {
    uint64_t total_events;
    uint64_t grants;
    uint64_t denials;
    uint64_t violations;
    
    // Top used capabilities
    struct {
        SecurityCapability capability;
        uint64_t use_count;
        char most_frequent_user[128];
    } top_capabilities[10];
    
    // Security incidents
    struct {
        uint64_t timestamp;
        char description[256];
        SecurityCapability capability;
        char entity[128];
    } *incidents;
    size_t incident_count;
} CapabilityAuditReport;

CapabilityAuditReport* capability_generate_audit_report(CapabilitySystem* system);
void capability_audit_report_destroy(CapabilityAuditReport* report);

// Integration with existing systems
Result_void_ptr capability_integrate_type_checker(CapabilityChecker* checker, TypeChecker* type_checker);
Result_void_ptr capability_integrate_security_framework(CapabilitySystem* system, 
                                                       SecurityContext* security_context);

// Capability annotations for compiler integration
#define REQUIRES_CAPABILITY(cap) __attribute__((annotate("requires_capability:" #cap)))
#define GRANTS_CAPABILITY(cap) __attribute__((annotate("grants_capability:" #cap)))
#define ACQUIRES_CAPABILITY(cap) __attribute__((annotate("acquires_capability:" #cap)))
#define RELEASES_CAPABILITY(cap) __attribute__((annotate("releases_capability:" #cap)))
#define NO_CAPABILITY_REQUIRED __attribute__((annotate("no_capability_required")))

// Helper macros for capability checking
#define CAPABILITY_CHECK(system, cap) \
    do { \
        Result_bool __result = capability_system_check((system), __func__, (cap)); \
        if (__result.is_error || !__result.value) { \
            return ERR_PTR(error_create(ERROR_CAPABILITY_SYS_DENIED, \
                "Capability " #cap " required for " __func__)); \
        } \
    } while(0)

#define WITH_CAPABILITY(system, cap, code) \
    do { \
        Result_void_ptr __acq = capability_acquire_dynamic((system), (cap), "temporary use"); \
        if (!__acq.is_error) { \
            code \
            capability_release_dynamic((system), (cap)); \
        } else { \
            return __acq; \
        } \
    } while(0)

// Standard capability sets
#define CAPABILITIES_NONE 0
#define CAPABILITIES_BASIC (CAP_MEMORY_ALLOC | CAP_READ_FILE)
#define CAPABILITIES_NETWORK (CAP_NETWORK_CONNECT | CAP_NETWORK_BIND | CAP_NETWORK_LISTEN)
#define CAPABILITIES_FILESYSTEM (CAP_READ_FILE | CAP_WRITE_FILE | CAP_EXECUTE_FILE)
#define CAPABILITIES_PROCESS (CAP_PROCESS_SPAWN | CAP_PROCESS_KILL)
#define CAPABILITIES_CRYPTO (CAP_CRYPTO_ENCRYPT | CAP_CRYPTO_DECRYPT | CAP_CRYPTO_SIGN | CAP_CRYPTO_VERIFY)
#define CAPABILITIES_PRIVILEGED (CAP_SYSTEM_CONFIG | CAP_USER_ADMIN | CAP_PRIVILEGE_ESCALATE)

// Error codes specific to capability system
#define ERROR_CAPABILITY_SYS_DENIED         0x7001
#define ERROR_CAPABILITY_EXPIRED            0x7002
#define ERROR_CAPABILITY_REVOKED            0x7003
#define ERROR_DELEGATION_DENIED             0x7004
#define ERROR_POLICY_VIOLATION              0x7005
#define ERROR_CONTEXT_MISMATCH              0x7006
#define ERROR_CAPABILITY_NOT_DELEGABLE      0x7007

#endif // GOO_CAPABILITY_SECURITY_H