#include "../../include/capability_security.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <regex.h>

// Utility functions
static uint64_t get_monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t get_monotonic_time_ms(void) {
    return get_monotonic_time_ns() / 1000000ULL;
}

static uint64_t generate_unique_id(void) {
    static _Atomic uint64_t counter = 1;
    return atomic_fetch_add(&counter, 1);
}

// Capability token operations
CapabilityToken* capability_token_create(SecurityCapability capability, const char* granted_to) {
    CapabilityToken* token = xcalloc(1, sizeof(CapabilityToken));
    if (!token) return NULL;
    
    token->token_id = generate_unique_id();
    token->capability = capability;
    token->grant_time = get_monotonic_time_ms();
    
    if (granted_to) {
        strncpy(token->granted_to, granted_to, sizeof(token->granted_to) - 1);
        token->granted_to[sizeof(token->granted_to) - 1] = '\0';
    }
    
    // Set default validity
    token->valid_from = token->grant_time;
    token->valid_until = token->grant_time + (24ULL * 60 * 60 * 1000); // 24 hours
    token->max_uses = 0; // Unlimited by default
    token->current_uses = 0;
    
    // Default delegation settings
    token->can_delegate = false;
    token->max_delegation_depth = 0;
    token->current_delegation_depth = 0;
    
    // Determine category
    if (capability & (CAP_READ_FILE | CAP_WRITE_FILE | CAP_EXECUTE_FILE)) {
        token->category = CAP_CATEGORY_FILE_SYSTEM;
    } else if (capability & (CAP_NETWORK_CONNECT | CAP_NETWORK_BIND | CAP_NETWORK_LISTEN)) {
        token->category = CAP_CATEGORY_NETWORK;
    } else if (capability & (CAP_PROCESS_SPAWN | CAP_PROCESS_KILL)) {
        token->category = CAP_CATEGORY_PROCESS;
    } else if (capability & (CAP_MEMORY_ALLOC | CAP_MEMORY_EXEC)) {
        token->category = CAP_CATEGORY_MEMORY;
    } else if (capability & (CAP_CRYPTO_ENCRYPT | CAP_CRYPTO_DECRYPT | CAP_CRYPTO_SIGN | CAP_CRYPTO_VERIFY)) {
        token->category = CAP_CATEGORY_CRYPTO;
    } else if (capability & (CAP_DATABASE_READ | CAP_DATABASE_WRITE)) {
        token->category = CAP_CATEGORY_DATABASE;
    } else if (capability & (CAP_SYSTEM_CONFIG | CAP_USER_ADMIN | CAP_TIME_MODIFY)) {
        token->category = CAP_CATEGORY_SYSTEM;
    } else {
        token->category = CAP_CATEGORY_USER_DEFINED;
    }
    
    return token;
}

void capability_token_destroy(CapabilityToken* token) {
    if (!token) return;
    
    // Free attributes
    CapabilityAttribute* attr = token->attributes;
    while (attr) {
        CapabilityAttribute* next = attr->next;
        free(attr);
        attr = next;
    }
    
    // Free string allocations
    free(token->allowed_contexts);
    free(token->denied_contexts);
    free(token->required_conditions);
    
    free(token);
}

// Capability requirement operations
CapabilityRequirement* capability_requirement_create(SecurityCapability capability, 
                                                    const char* description) {
    CapabilityRequirement* req = xcalloc(1, sizeof(CapabilityRequirement));
    if (!req) return NULL;
    
    req->required_capability = capability;
    req->is_mandatory = true;
    req->is_runtime_checked = true;
    
    if (description) {
        strncpy(req->description, description, sizeof(req->description) - 1);
        req->description[sizeof(req->description) - 1] = '\0';
    }
    
    return req;
}

void capability_requirement_destroy(CapabilityRequirement* requirement) {
    if (!requirement) return;
    
    free(requirement->alternatives);
    free(requirement);
}

// Capability policy operations
CapabilityPolicy* capability_policy_create(const char* name) {
    CapabilityPolicy* policy = xcalloc(1, sizeof(CapabilityPolicy));
    if (!policy) return NULL;
    
    if (name) {
        strncpy(policy->policy_name, name, sizeof(policy->policy_name) - 1);
        policy->policy_name[sizeof(policy->policy_name) - 1] = '\0';
    }
    
    // Initialize rule storage
    policy->rule_capacity = 50;
    policy->rules = calloc(policy->rule_capacity, sizeof(*policy->rules));
    if (!policy->rules) {
        free(policy);
        return NULL;
    }
    
    // Default settings
    policy->allow_delegation = true;
    policy->max_delegation_depth = 3;
    policy->default_validity_duration_ms = 24ULL * 60 * 60 * 1000; // 24 hours
    policy->max_validity_duration_ms = 7ULL * 24 * 60 * 60 * 1000; // 7 days
    
    return policy;
}

void capability_policy_destroy(CapabilityPolicy* policy) {
    if (!policy) return;
    
    free(policy->rules);
    free(policy);
}

CapabilityPolicy* capability_policy_create_strict(void) {
    CapabilityPolicy* policy = capability_policy_create("strict");
    if (!policy) return NULL;
    
    strncpy(policy->description, "Strict capability policy - minimal permissions", 
            sizeof(policy->description) - 1);
    
    // Very limited default capabilities
    policy->default_function_capabilities = CAP_MEMORY_ALLOC;
    policy->default_module_capabilities = CAP_MEMORY_ALLOC | CAP_READ_FILE;
    policy->default_thread_capabilities = CAP_MEMORY_ALLOC;
    policy->default_actor_capabilities = CAP_MEMORY_ALLOC;
    
    // Many capabilities are forbidden or privileged
    policy->forbidden_capabilities = CAP_KERNEL_MODULE | CAP_SANDBOX_ESCAPE | CAP_PRIVILEGE_ESCALATE;
    policy->privileged_capabilities = CAP_SYSTEM_CONFIG | CAP_USER_ADMIN | CAP_HARDWARE_ACCESS |
                                     CAP_PROCESS_KILL | CAP_TIME_MODIFY;
    policy->audit_required_capabilities = 0xFFFFFFFF; // Audit everything
    
    // Limited delegation
    policy->allow_delegation = false;
    policy->max_delegation_depth = 0;
    
    // Short validity periods
    policy->default_validity_duration_ms = 1ULL * 60 * 60 * 1000; // 1 hour
    policy->max_validity_duration_ms = 24ULL * 60 * 60 * 1000; // 24 hours
    policy->require_periodic_renewal = true;
    policy->renewal_interval_ms = 30ULL * 60 * 1000; // 30 minutes
    
    return policy;
}

CapabilityPolicy* capability_policy_create_moderate(void) {
    CapabilityPolicy* policy = capability_policy_create("moderate");
    if (!policy) return NULL;
    
    strncpy(policy->description, "Moderate capability policy - balanced security", 
            sizeof(policy->description) - 1);
    
    // Reasonable default capabilities
    policy->default_function_capabilities = CAPABILITIES_BASIC;
    policy->default_module_capabilities = CAPABILITIES_BASIC | CAPABILITIES_FILESYSTEM;
    policy->default_thread_capabilities = CAPABILITIES_BASIC;
    policy->default_actor_capabilities = CAPABILITIES_BASIC | CAP_NETWORK_CONNECT;
    
    // Some capabilities are forbidden
    policy->forbidden_capabilities = CAP_KERNEL_MODULE | CAP_SANDBOX_ESCAPE;
    policy->privileged_capabilities = CAP_PRIVILEGE_ESCALATE | CAP_SYSTEM_CONFIG | CAP_USER_ADMIN;
    policy->audit_required_capabilities = policy->privileged_capabilities | CAPABILITIES_CRYPTO;
    
    // Moderate delegation
    policy->allow_delegation = true;
    policy->max_delegation_depth = 2;
    policy->non_delegatable_capabilities = policy->privileged_capabilities;
    
    return policy;
}

CapabilityPolicy* capability_policy_create_permissive(void) {
    CapabilityPolicy* policy = capability_policy_create("permissive");
    if (!policy) return NULL;
    
    strncpy(policy->description, "Permissive capability policy - maximum flexibility", 
            sizeof(policy->description) - 1);
    
    // Generous default capabilities
    policy->default_function_capabilities = CAPABILITIES_BASIC | CAPABILITIES_FILESYSTEM;
    policy->default_module_capabilities = 0xFFFFFFFF & ~(CAP_PRIVILEGE_ESCALATE | CAP_KERNEL_MODULE);
    policy->default_thread_capabilities = CAPABILITIES_BASIC | CAPABILITIES_NETWORK;
    policy->default_actor_capabilities = CAPABILITIES_BASIC | CAPABILITIES_NETWORK | CAPABILITIES_PROCESS;
    
    // Very few restrictions
    policy->forbidden_capabilities = 0;
    policy->privileged_capabilities = CAP_PRIVILEGE_ESCALATE;
    policy->audit_required_capabilities = CAP_PRIVILEGE_ESCALATE | CAP_KERNEL_MODULE;
    
    // Liberal delegation
    policy->allow_delegation = true;
    policy->max_delegation_depth = 5;
    policy->non_delegatable_capabilities = 0;
    
    // Long validity periods
    policy->default_validity_duration_ms = 30ULL * 24 * 60 * 60 * 1000; // 30 days
    policy->max_validity_duration_ms = 365ULL * 24 * 60 * 60 * 1000; // 1 year
    policy->require_periodic_renewal = false;
    
    return policy;
}

// Capability context operations
CapabilityContext* capability_context_create(const char* name, int context_type) {
    CapabilityContext* context = xcalloc(1, sizeof(CapabilityContext));
    if (!context) return NULL;
    
    if (name) {
        strncpy(context->context_name, name, sizeof(context->context_name) - 1);
        context->context_name[sizeof(context->context_name) - 1] = '\0';
    }
    
    context->context_type = context_type;
    
    // Initialize token storage
    context->token_capacity = 10;
    context->active_tokens = calloc(context->token_capacity, sizeof(CapabilityToken*));
    if (!context->active_tokens) {
        free(context);
        return NULL;
    }
    
    // Initialize child storage
    context->child_capacity = 5;
    context->children = calloc(context->child_capacity, sizeof(CapabilityContext*));
    if (!context->children) {
        free(context->active_tokens);
        free(context);
        return NULL;
    }
    
    pthread_mutex_init(&context->context_mutex, NULL);
    
    return context;
}

void capability_context_destroy(CapabilityContext* context) {
    if (!context) return;
    
    pthread_mutex_destroy(&context->context_mutex);
    
    // Don't destroy tokens - they're managed by the system
    free(context->active_tokens);
    
    // Recursively destroy children
    for (size_t i = 0; i < context->child_count; i++) {
        capability_context_destroy(context->children[i]);
    }
    free(context->children);
    
    free(context);
}

// Capability checker operations
CapabilityChecker* capability_checker_create(CapabilitySystem* system) {
    CapabilityChecker* checker = xcalloc(1, sizeof(CapabilityChecker));
    if (!checker) return NULL;
    
    checker->capability_system = system;
    
    // Initialize binding storage
    checker->binding_capacity = 100;
    checker->bindings = calloc(checker->binding_capacity, sizeof(CapabilityBinding*));
    if (!checker->bindings) {
        free(checker);
        return NULL;
    }
    
    // Set default configuration
    checker->config.enable_inference = true;
    checker->config.enable_static_checking = true;
    checker->config.enable_runtime_checking = true;
    checker->config.strict_mode = false;
    checker->config.generate_runtime_checks = true;
    checker->config.annotate_ast = true;
    
    pthread_mutex_init(&checker->checker_mutex, NULL);
    
    return checker;
}

void capability_checker_destroy(CapabilityChecker* checker) {
    if (!checker) return;
    
    pthread_mutex_destroy(&checker->checker_mutex);
    
    // Clean up bindings
    for (size_t i = 0; i < checker->binding_count; i++) {
        CapabilityBinding* binding = checker->bindings[i];
        if (binding) {
            // Clean up requirements
            CapabilityRequirement* req = binding->requirements;
            while (req) {
                CapabilityRequirement* next = req->next;
                capability_requirement_destroy(req);
                req = next;
            }
            free(binding);
        }
    }
    free(checker->bindings);
    
    // Clean up analysis state
    free(checker->analysis_state.violations);
    
    // Clean up inferred requirements
    CapabilityRequirement* req = checker->analysis_state.inferred_requirements;
    while (req) {
        CapabilityRequirement* next = req->next;
        capability_requirement_destroy(req);
        req = next;
    }
    
    free(checker);
}

// Main capability system operations
CapabilitySystem* capability_system_create(SecurityContext* security_context) {
    CapabilitySystem* system = xcalloc(1, sizeof(CapabilitySystem));
    if (!system) return NULL;
    
    system->security_context = security_context;
    
    // Initialize token storage
    system->token_capacity = 1000;
    system->tokens = calloc(system->token_capacity, sizeof(CapabilityToken*));
    if (!system->tokens) {
        free(system);
        return NULL;
    }
    
    // Initialize policy storage
    system->policy_capacity = 10;
    system->policies = calloc(system->policy_capacity, sizeof(CapabilityPolicy*));
    if (!system->policies) {
        free(system->tokens);
        free(system);
        return NULL;
    }
    
    // Initialize context storage
    system->context_capacity = 100;
    system->all_contexts = calloc(system->context_capacity, sizeof(CapabilityContext*));
    if (!system->all_contexts) {
        free(system->policies);
        free(system->tokens);
        free(system);
        return NULL;
    }
    
    // Create global context
    system->global_context = capability_context_create("global", CONTEXT_GLOBAL);
    if (!system->global_context) {
        free(system->all_contexts);
        free(system->policies);
        free(system->tokens);
        free(system);
        return NULL;
    }
    
    // Initialize audit log
    system->audit_capacity = 10000;
    system->audit_log = calloc(system->audit_capacity, sizeof(*system->audit_log));
    if (!system->audit_log) {
        capability_context_destroy(system->global_context);
        free(system->all_contexts);
        free(system->policies);
        free(system->tokens);
        free(system);
        return NULL;
    }
    
    // Create capability checker
    system->checker = capability_checker_create(system);
    if (!system->checker) {
        free(system->audit_log);
        capability_context_destroy(system->global_context);
        free(system->all_contexts);
        free(system->policies);
        free(system->tokens);
        free(system);
        return NULL;
    }
    
    // Set defaults
    system->audit_enabled = true;
    system->strict_enforcement = true;
    system->allow_capability_escalation = false;
    
    pthread_mutex_init(&system->system_mutex, NULL);
    
    return system;
}

void capability_system_destroy(CapabilitySystem* system) {
    if (!system) return;
    
    pthread_mutex_destroy(&system->system_mutex);
    
    // Clean up tokens
    for (size_t i = 0; i < system->token_count; i++) {
        capability_token_destroy(system->tokens[i]);
    }
    free(system->tokens);
    
    // Clean up policies
    for (size_t i = 0; i < system->policy_count; i++) {
        capability_policy_destroy(system->policies[i]);
    }
    free(system->policies);
    
    // Clean up contexts
    for (size_t i = 0; i < system->context_count; i++) {
        capability_context_destroy(system->all_contexts[i]);
    }
    free(system->all_contexts);
    
    // Clean up global context (if not in all_contexts)
    if (system->global_context) {
        bool found = false;
        for (size_t i = 0; i < system->context_count; i++) {
            if (system->all_contexts[i] == system->global_context) {
                found = true;
                break;
            }
        }
        if (!found) {
            capability_context_destroy(system->global_context);
        }
    }
    
    // Clean up authorities
    free(system->authorities);
    
    // Clean up audit log
    free(system->audit_log);
    
    // Clean up checker
    capability_checker_destroy(system->checker);
    
    free(system);
}

Result_void_ptr capability_system_initialize(CapabilitySystem* system) {
    if (!system) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid capability system";
        }
        return ERR_PTR(err);
    }
    
    pthread_mutex_lock(&system->system_mutex);
    
    // Add global context to all contexts
    if (system->context_count < system->context_capacity) {
        system->all_contexts[system->context_count++] = system->global_context;
    }
    
    // Create and set default policy based on security context policy
    CapabilityPolicy* default_policy = NULL;
    if (system->security_context && system->security_context->policy == SECURITY_POLICY_STRICT) {
        default_policy = capability_policy_create_strict();
    } else if (system->security_context && system->security_context->policy == SECURITY_POLICY_PERMISSIVE) {
        default_policy = capability_policy_create_permissive();
    } else {
        default_policy = capability_policy_create_moderate();
    }
    
    if (default_policy) {
        if (system->policy_count < system->policy_capacity) {
            system->policies[system->policy_count++] = default_policy;
            system->active_policy = default_policy;
        } else {
            capability_policy_destroy(default_policy);
        }
    }
    
    system->is_initialized = true;
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return OK_PTR(system);
}

// Core capability operations
Result_void_ptr capability_system_grant(CapabilitySystem* system, SecurityCapability capability, 
                                const char* entity, uint64_t validity_ms) {
    if (!system || !entity) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid parameters";
        }
        return ERR_PTR(err);
    }
    
    pthread_mutex_lock(&system->system_mutex);
    
    // Check if capability is forbidden
    if (system->active_policy && 
        (capability & system->active_policy->forbidden_capabilities)) {
        pthread_mutex_unlock(&system->system_mutex);
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_CAPABILITY_SYS_DENIED;
            err->message = "Capability is forbidden by policy";
        }
        return ERR_PTR(err);
    }
    
    // Create token
    CapabilityToken* token = capability_token_create(capability, entity);
    if (!token) {
        pthread_mutex_unlock(&system->system_mutex);
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_OUT_OF_MEMORY;
            err->message = "Failed to create capability token";
        }
        return ERR_PTR(err);
    }
    
    // Set validity
    if (validity_ms > 0) {
        token->valid_until = token->valid_from + validity_ms;
    } else if (system->active_policy) {
        token->valid_until = token->valid_from + system->active_policy->default_validity_duration_ms;
    }
    
    // Add to system
    if (system->token_count < system->token_capacity) {
        system->tokens[system->token_count++] = token;
        system->stats.total_grants++;
        system->stats.active_tokens++;
        
        // Audit
        if (system->audit_enabled && system->audit_count < system->audit_capacity) {
            system->audit_log[system->audit_count++] = (typeof(system->audit_log[0])){
                .event_id = generate_unique_id(),
                .timestamp = get_monotonic_time_ms(),
                .event_type = AUDIT_GRANT,
                .token_id = token->token_id,
                .capability = capability,
                .success = true
            };
            strncpy(system->audit_log[system->audit_count - 1].entity, entity, 127);
        }
        
        pthread_mutex_unlock(&system->system_mutex);
        return OK_PTR(token);
    } else {
        capability_token_destroy(token);
        pthread_mutex_unlock(&system->system_mutex);
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_OUT_OF_MEMORY;
            err->message = "Token storage full";
        }
        return ERR_PTR(err);
    }
}

Result_bool capability_system_check(CapabilitySystem* system, const char* entity, SecurityCapability capability) {
    if (!system || !entity) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid parameters";
        }
        return ERR(bool, err);
    }
    
    pthread_mutex_lock(&system->system_mutex);
    
    uint64_t current_time = get_monotonic_time_ms();
    bool has_capability = false;
    
    // Check all tokens for the entity
    for (size_t i = 0; i < system->token_count; i++) {
        CapabilityToken* token = system->tokens[i];
        if (!token || token->is_revoked) continue;
        
        // Check if token belongs to entity
        if (strcmp(token->granted_to, entity) != 0) continue;
        
        // Check if token has the required capability
        if (!(token->capability & capability)) continue;
        
        // Check validity
        if (current_time < token->valid_from || current_time > token->valid_until) continue;
        
        // Check usage limits
        if (token->max_uses > 0 && token->current_uses >= token->max_uses) continue;
        
        // Found valid token
        has_capability = true;
        break;
    }
    
    // Audit the check
    if (system->audit_enabled && system->audit_count < system->audit_capacity) {
        system->audit_log[system->audit_count++] = (typeof(system->audit_log[0])){
            .event_id = generate_unique_id(),
            .timestamp = current_time,
            .event_type = has_capability ? AUDIT_USE : AUDIT_DENY,
            .capability = capability,
            .success = has_capability
        };
        strncpy(system->audit_log[system->audit_count - 1].entity, entity, 127);
        
        if (!has_capability) {
            system->stats.total_denials++;
        }
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return OK(bool, has_capability);
}

// Stub implementations for complex functions
Result_void_ptr capability_check_ast(CapabilityChecker* checker, ASTNode* root) {
    if (!checker || !root) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid parameters";
        }
        return ERR_PTR(err);
    }
    
    // This would perform full AST traversal for capability checking
    // For now, return success to indicate the framework is set up
    
    return OK_PTR(checker);
}

Result_void_ptr capability_check_function(CapabilityChecker* checker, ASTNode* function_node) {
    if (!checker || !function_node) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid parameters";
        }
        return ERR_PTR(err);
    }
    
    // This would analyze a function for capability requirements
    // For now, return success
    
    return OK_PTR(checker);
}