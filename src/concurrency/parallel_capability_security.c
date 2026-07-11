#include "../../include/parallel_capability_security.h"
#include "../../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// Global capability ID counter
static atomic_uint_fast64_t g_capability_id_counter = 1;
static atomic_uint_fast64_t g_token_id_counter = 1;

// Get high-precision timestamp for capability tracking
uint64_t get_capability_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Create a new capability token
CapabilityToken* capability_token_create(SecurityLevel level, uint64_t task_id) {
    CapabilityToken* token = xcalloc(1, sizeof(CapabilityToken));
    if (!token) return NULL;
    
    token->token_id = atomic_fetch_add(&g_token_id_counter, 1);
    token->security_level = level;
    token->task_id = task_id;
    token->worker_id = 0; // Will be set by task context
    token->is_active = true;
    token->creation_time = get_capability_timestamp();
    token->last_access_time = token->creation_time;
    
    // Allocate initial capability array
    token->capability_capacity = 16;
    token->capabilities = calloc(token->capability_capacity, sizeof(SecurityCapability*));
    if (!token->capabilities) {
        free(token);
        return NULL;
    }
    
    atomic_init(&token->total_accesses, 0);
    atomic_init(&token->violations, 0);
    
    return token;
}

// Destroy capability token
void capability_token_destroy(CapabilityToken* token) {
    if (!token) return;
    
    // Free all capabilities
    for (size_t i = 0; i < token->capability_count; i++) {
        security_capability_destroy(token->capabilities[i]);
    }
    
    free(token->capabilities);
    free(token);
}

// Grant a capability to a token
bool capability_token_grant(CapabilityToken* token, SecurityCapability* capability) {
    if (!token || !capability || !token->is_active) return false;
    
    // Check if we need to expand the capabilities array
    if (token->capability_count >= token->capability_capacity) {
        size_t new_capacity = token->capability_capacity * 2;
        SecurityCapability** new_caps = realloc(token->capabilities, 
                                               new_capacity * sizeof(SecurityCapability*));
        if (!new_caps) return false;
        
        token->capabilities = new_caps;
        token->capability_capacity = new_capacity;
    }
    
    // Add the capability
    token->capabilities[token->capability_count++] = capability;
    return true;
}

// Check if token has a specific capability
bool capability_token_has_capability(CapabilityToken* token, CapabilityType type, void* resource) {
    if (!token || !token->is_active) return false;
    
    atomic_fetch_add(&token->total_accesses, 1);
    token->last_access_time = get_capability_timestamp();
    
    for (size_t i = 0; i < token->capability_count; i++) {
        SecurityCapability* cap = token->capabilities[i];
        
        if (!security_capability_is_valid(cap)) continue;
        
        // Check capability type
        if (!(cap->type & type)) continue;
        
        // Check resource match (if specified)
        if (resource && cap->resource_ptr && cap->resource_ptr != resource) continue;
        
        // Check access limits
        if (cap->max_accesses > 0) {
            uint32_t current_accesses = atomic_fetch_add(&cap->access_count, 1);
            if (current_accesses >= cap->max_accesses) {
                atomic_fetch_add(&token->violations, 1);
                return false;
            }
        }
        
        return true;
    }
    
    atomic_fetch_add(&token->violations, 1);
    return false;
}

// Create a new security capability
SecurityCapability* security_capability_create(CapabilityType type, SecurityLevel required_level, 
                                              void* resource, size_t resource_size) {
    SecurityCapability* cap = xcalloc(1, sizeof(SecurityCapability));
    if (!cap) return NULL;
    
    cap->capability_id = atomic_fetch_add(&g_capability_id_counter, 1);
    cap->type = type;
    cap->required_level = required_level;
    cap->resource_ptr = resource;
    cap->resource_size = resource_size;
    cap->max_accesses = 0; // Unlimited by default
    cap->can_delegate = true;
    cap->is_valid = true;
    cap->creation_time = get_capability_timestamp();
    cap->expiry_time = 0; // Never expires by default
    
    atomic_init(&cap->access_count, 0);
    
    // Create description
    char* desc = malloc(256);
    if (desc) {
        snprintf(desc, 256, "Capability %llu: %s on resource %p", 
                cap->capability_id, capability_type_to_string(type), resource);
        cap->description = desc;
    }
    
    return cap;
}

// Destroy security capability
void security_capability_destroy(SecurityCapability* capability) {
    if (!capability) return;
    
    free(capability->description);
    free(capability);
}

// Check if capability is valid
bool security_capability_is_valid(SecurityCapability* capability) {
    if (!capability || !capability->is_valid) return false;
    
    // Check expiry time
    if (capability->expiry_time > 0) {
        uint64_t now = get_capability_timestamp();
        if (now > capability->expiry_time) {
            capability->is_valid = false;
            return false;
        }
    }
    
    return true;
}

// Create memory region
MemoryRegion* memory_region_create(void* base, size_t size, uint32_t access_flags, const char* name) {
    MemoryRegion* region = xcalloc(1, sizeof(MemoryRegion));
    if (!region) return NULL;
    
    region->base_address = base;
    region->size = size;
    region->access_flags = access_flags;
    region->region_id = atomic_fetch_add(&g_capability_id_counter, 1);
    
    if (name) {
        region->region_name = strdup(name);
    }
    
    atomic_init(&region->access_count, 0);
    
    return region;
}

// Destroy memory region
void memory_region_destroy(MemoryRegion* region) {
    if (!region) return;
    
    free(region->region_name);
    free(region);
}

// Check memory region access
bool memory_region_check_access(MemoryRegion* region, void* address, size_t size, CapabilityType access_type) {
    if (!region || !address) return false;
    
    // Check if access type is allowed
    if (!(region->access_flags & access_type)) return false;
    
    // Check address range
    uintptr_t addr = (uintptr_t)address;
    uintptr_t base = (uintptr_t)region->base_address;
    
    if (addr < base || addr + size > base + region->size) return false;
    
    atomic_fetch_add(&region->access_count, 1);
    return true;
}

// Validate memory access with capabilities
bool validate_memory_access(SecureTaskContext* ctx, void* address, size_t size, CapabilityType access_type) {
    if (!ctx || !ctx->token) return false;
    
    atomic_fetch_add(&ctx->security_checks, 1);
    uint64_t start_time = get_capability_timestamp();
    
    // Check token capability
    bool has_general_cap = capability_token_has_capability(ctx->token, access_type, NULL);
    
    // Check specific memory regions
    bool region_access = false;
    for (size_t i = 0; i < ctx->region_count; i++) {
        if (memory_region_check_access(ctx->accessible_regions[i], address, size, access_type)) {
            region_access = true;
            break;
        }
    }
    
    uint64_t end_time = get_capability_timestamp();
    atomic_fetch_add(&ctx->security_check_time_ns, end_time - start_time);
    
    bool access_granted = has_general_cap || region_access;
    
    if (ctx->audit_all_accesses) {
        audit_capability_access(ctx, access_type, access_granted);
    }
    
    return access_granted;
}

// Validate capability access
bool validate_capability_access(SecureTaskContext* ctx, CapabilityType required_cap, void* resource) {
    if (!ctx || !ctx->token) return false;
    
    atomic_fetch_add(&ctx->security_checks, 1);
    uint64_t start_time = get_capability_timestamp();
    
    bool access_granted = capability_token_has_capability(ctx->token, required_cap, resource);
    
    uint64_t end_time = get_capability_timestamp();
    atomic_fetch_add(&ctx->security_check_time_ns, end_time - start_time);
    
    if (ctx->audit_all_accesses) {
        audit_capability_access(ctx, required_cap, access_granted);
    }
    
    return access_granted;
}

// Record security violation
void record_security_violation(SecureTaskContext* ctx, CapabilityType attempted_cap, const char* details) {
    if (!ctx) return;
    
    if (ctx->token) {
        atomic_fetch_add(&ctx->token->violations, 1);
    }
    
    if (ctx->on_violation) {
        ctx->on_violation(ctx->base_context.task_id, attempted_cap, details);
    }
    
    // Log violation for audit
    if (ctx->audit_all_accesses) {
        printf("SECURITY VIOLATION: Task %llu attempted %s - %s\n", 
               ctx->base_context.task_id, capability_type_to_string(attempted_cap), details);
    }
}

// Audit capability access
void audit_capability_access(SecureTaskContext* ctx, CapabilityType cap_type, bool granted) {
    if (!ctx) return;
    
    printf("AUDIT: Task %llu %s access to %s\n", 
           ctx->base_context.task_id, 
           granted ? "GRANTED" : "DENIED",
           capability_type_to_string(cap_type));
}

// Secure task wrapper arguments
typedef struct SecureTaskArgs {
    ParallelForTaskArgs base_args;
    CapabilitySecureParallelConfig* security_config;
    CapabilityToken* task_token;
    MemoryRegion** accessible_regions;
    size_t region_count;
} SecureTaskArgs;

// Secure task wrapper function
Result_void_ptr capability_secure_task_wrapper(TaskContext* task_ctx, void* args) {
    SecureTaskArgs* secure_args = (SecureTaskArgs*)args;
    
    if (!secure_args || !secure_args->base_args.func) {
        return OK_PTR(NULL);
    }
    
    // Create secure task context
    SecureTaskContext secure_ctx = {
        .base_context = *task_ctx,
        .token = secure_args->task_token,
        .task_security_level = secure_args->security_config->default_security_level,
        .accessible_regions = secure_args->accessible_regions,
        .region_count = secure_args->region_count,
        .on_violation = secure_args->security_config->on_security_violation,
        .strict_enforcement = secure_args->security_config->strict_isolation,
        .audit_all_accesses = secure_args->security_config->enable_access_auditing
    };
    
    atomic_init(&secure_ctx.security_checks, 0);
    atomic_init(&secure_ctx.security_check_time_ns, 0);
    
    // Execute the parallel for range with capability checking
    for (size_t i = secure_args->base_args.start; i < secure_args->base_args.end; i++) {
        CHECK_CANCELLATION(task_ctx);
        
        // Update progress
        int progress = (int)((i - secure_args->base_args.start + 1) * 100 / 
                            (secure_args->base_args.end - secure_args->base_args.start));
        UPDATE_PROGRESS(task_ctx, progress, "Processing items with capability security");
        
        // Validate basic compute capability before executing user function
        if (secure_args->security_config->enable_capability_checking) {
            if (!validate_capability_access(&secure_ctx, CAP_MEMORY_READ | CAP_MEMORY_WRITE, NULL)) {
                if (secure_args->security_config->terminate_on_violation) {
                    Error* error = security_create_access_denied_error(CAP_MEMORY_READ | CAP_MEMORY_WRITE, NULL);
                    return ERR_PTR(error);
                }
            }
        }
        
        // Call the user function with security context available
        Result_void_ptr result = secure_args->base_args.func(i, secure_args->base_args.user_context);
        
        if (result.is_error) {
            return result;
        }
    }
    
    return OK_PTR(NULL);
}

// Work-stealing capability secure parallel for (stub for minimal build)
Result_void_ptr capability_secure_work_stealing_parallel_for(
    WorkStealingScope* scope,
    CapabilitySecureParallelConfig config,
    ParallelForFunction function,
    void* context) {
    
    // For minimal build, delegate to regular secure parallel for
    return capability_secure_parallel_for(&scope->base_scope, config, function, context);
}

// Capability-secure parallel for implementation
Result_void_ptr capability_secure_parallel_for(
    TaskScope* scope,
    CapabilitySecureParallelConfig config,
    ParallelForFunction function,
    void* context) {
    
    if (!scope || !function) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid secure parallel for parameters"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Validate configuration
    if (config.base_config.start_index >= config.base_config.end_index) {
        return OK_PTR(NULL);
    }
    
    size_t total_items = config.base_config.end_index - config.base_config.start_index;
    size_t chunk_size = config.base_config.chunk_size;
    if (chunk_size == 0) {
        chunk_size = (total_items + config.base_config.max_workers - 1) / config.base_config.max_workers;
        if (chunk_size < 1) chunk_size = 1;
    }
    
    // Create secure task arguments and tasks
    size_t task_count = 0;
    SecureTaskArgs* all_secure_args = NULL;
    ConcurrentTask** tasks = NULL;
    
    size_t max_tasks = (total_items + chunk_size - 1) / chunk_size;
    
    all_secure_args = calloc(max_tasks, sizeof(SecureTaskArgs));
    tasks = calloc(max_tasks, sizeof(ConcurrentTask*));
    
    if (!all_secure_args || !tasks) {
        free(all_secure_args);
        free(tasks);
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Memory allocation failed"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Create and submit tasks
    for (size_t start = config.base_config.start_index; 
         start < config.base_config.end_index; 
         start += chunk_size) {
        
        size_t end = start + chunk_size;
        if (end > config.base_config.end_index) {
            end = config.base_config.end_index;
        }
        
        // Create capability token for this task
        CapabilityToken* task_token = capability_token_create(config.default_security_level, task_count);
        
        // Grant default capabilities
        if (config.granted_capabilities) {
            for (size_t i = 0; i < config.granted_capability_count; i++) {
                capability_token_grant(task_token, config.granted_capabilities[i]);
            }
        } else {
            // Create basic compute capabilities
            SecurityCapability* read_cap = security_capability_create(
                config.default_capabilities & (CAP_MEMORY_READ | CAP_MEMORY_WRITE),
                config.default_security_level, NULL, 0);
            if (read_cap) {
                capability_token_grant(task_token, read_cap);
            }
        }
        
        // Initialize secure task arguments
        SecureTaskArgs* secure_args = &all_secure_args[task_count];
        secure_args->base_args = (ParallelForTaskArgs){
            .start = start,
            .end = end,
            .func = function,
            .user_context = context
        };
        
        secure_args->security_config = &config;
        secure_args->task_token = task_token;
        secure_args->accessible_regions = config.allowed_memory_regions;
        secure_args->region_count = config.memory_region_count;
        
        // Create task with secure wrapper
        char task_name[64];
        snprintf(task_name, sizeof(task_name), "secure_parallel_for_%zu_%zu", start, end);
        
        ConcurrentTask* task = task_create(scope, capability_secure_task_wrapper,
                                         secure_args, sizeof(SecureTaskArgs), task_name);
        if (task) {
            task->priority = config.base_config.priority;
            tasks[task_count] = task;
            
            Result_void_ptr submit_result = task_submit(task);
            if (submit_result.is_error) {
                // Cleanup on failure
                for (size_t i = 0; i < task_count; i++) {
                    if (tasks[i]) {
                        task_cancel(tasks[i], CANCEL_GRACEFUL);
                    }
                    capability_token_destroy(all_secure_args[i].task_token);
                }
                free(all_secure_args);
                free(tasks);
                return submit_result;
            }
            task_count++;
        }
    }
    
    // Wait for all tasks to complete
    Result_void_ptr first_error = OK_PTR(NULL);
    uint64_t total_violations = 0;
    uint64_t total_security_checks = 0;
    
    for (size_t i = 0; i < task_count; i++) {
        if (tasks[i]) {
            Result_void_ptr wait_result = task_wait(tasks[i], UINT64_MAX);
            if (wait_result.is_error && first_error.is_error == false) {
                first_error = wait_result;
            }
            
            // Collect security statistics
            if (all_secure_args[i].task_token) {
                total_violations += atomic_load(&all_secure_args[i].task_token->violations);
            }
        }
    }
    
    // Report security statistics
    if (config.enable_access_auditing) {
        printf("Security Report:\n");
        printf("  Total tasks: %zu\n", task_count);
        printf("  Security violations: %llu\n", total_violations);
        printf("  Security checks performed: %llu\n", total_security_checks);
    }
    
    // Cleanup
    for (size_t i = 0; i < task_count; i++) {
        capability_token_destroy(all_secure_args[i].task_token);
    }
    free(all_secure_args);
    free(tasks);
    
    return first_error;
}

// Configuration presets
CapabilitySecureParallelConfig capability_secure_config_default(void) {
    return (CapabilitySecureParallelConfig) {
        .base_config = {
            .start_index = 0,
            .end_index = 0,
            .chunk_size = 0,
            .max_workers = 8,
            .priority = TASK_PRIORITY_NORMAL
        },
        .default_security_level = SECURITY_LEVEL_STANDARD,
        .enable_capability_checking = true,
        .enable_access_auditing = false,
        .strict_isolation = false,
        .default_capabilities = CAP_MEMORY_READ | CAP_MEMORY_WRITE,
        .terminate_on_violation = false,
        .log_all_accesses = false,
        .track_capability_usage = true,
        .on_security_violation = NULL,
        .on_access_denied = NULL,
        .allowed_memory_regions = NULL,
        .memory_region_count = 0,
        .granted_capabilities = NULL,
        .granted_capability_count = 0,
        .audit_log_path = NULL
    };
}

CapabilitySecureParallelConfig capability_secure_config_strict(void) {
    CapabilitySecureParallelConfig config = capability_secure_config_default();
    config.default_security_level = SECURITY_LEVEL_RESTRICTED;
    config.enable_access_auditing = true;
    config.strict_isolation = true;
    config.terminate_on_violation = true;
    config.log_all_accesses = true;
    config.default_capabilities = CAP_MEMORY_READ; // Read-only by default
    return config;
}

CapabilitySecureParallelConfig capability_secure_config_untrusted(void) {
    CapabilitySecureParallelConfig config = capability_secure_config_strict();
    config.default_security_level = SECURITY_LEVEL_UNTRUSTED;
    config.default_capabilities = 0; // No capabilities by default
    return config;
}

// Utility functions
const char* capability_type_to_string(CapabilityType type) {
    switch (type) {
        case CAP_MEMORY_READ: return "MEMORY_READ";
        case CAP_MEMORY_WRITE: return "MEMORY_WRITE";
        case CAP_FILE_READ: return "FILE_READ";
        case CAP_FILE_WRITE: return "FILE_WRITE";
        case CAP_NETWORK_READ: return "NETWORK_READ";
        case CAP_NETWORK_WRITE: return "NETWORK_WRITE";
        case CAP_SYSTEM_CALL: return "SYSTEM_CALL";
        case CAP_THREAD_CREATE: return "THREAD_CREATE";
        case CAP_TIMER_ACCESS: return "TIMER_ACCESS";
        case CAP_IPC_SEND: return "IPC_SEND";
        case CAP_IPC_RECEIVE: return "IPC_RECEIVE";
        case CAP_DEVICE_ACCESS: return "DEVICE_ACCESS";
        case CAP_DEBUG_ACCESS: return "DEBUG_ACCESS";
        case CAP_CRYPTO_OPERATIONS: return "CRYPTO_OPERATIONS";
        case CAP_ALL: return "ALL_CAPABILITIES";
        default: return "UNKNOWN";
    }
}

const char* security_level_to_string(SecurityLevel level) {
    switch (level) {
        case SECURITY_LEVEL_UNTRUSTED: return "UNTRUSTED";
        case SECURITY_LEVEL_RESTRICTED: return "RESTRICTED";
        case SECURITY_LEVEL_STANDARD: return "STANDARD";
        case SECURITY_LEVEL_PRIVILEGED: return "PRIVILEGED";
        case SECURITY_LEVEL_SYSTEM: return "SYSTEM";
        default: return "UNKNOWN";
    }
}

// Error creation helpers
Error* security_create_access_denied_error(CapabilityType cap_type, void* resource) {
    Error* error = xmalloc(sizeof(Error));
    if (!error) return NULL;
    
    char* message = malloc(256);
    if (!message) {
        free(error);
        return NULL;
    }
    
    snprintf(message, 256, "Access denied: missing capability %s for resource %p", 
             capability_type_to_string(cap_type), resource);
    
    *error = (Error){
        .code = ERROR_OPERATION_FAILED,
        .severity = ERROR_SEVERITY_ERROR,
        .category = ERROR_CATEGORY_RUNTIME,
        .message = message,
        .hint = strdup("Check task capabilities and security configuration"),
        .location = (SourceLocation){0},
        .next = NULL
    };
    
    return error;
}

Error* security_create_memory_violation_error(void* address, size_t size) {
    Error* error = xmalloc(sizeof(Error));
    if (!error) return NULL;
    
    char* message = malloc(256);
    if (!message) {
        free(error);
        return NULL;
    }
    
    snprintf(message, 256, "Memory access violation: address %p, size %zu", address, size);
    
    *error = (Error){
        .code = ERROR_BUFFER_OVERFLOW,
        .severity = ERROR_SEVERITY_ERROR,
        .category = ERROR_CATEGORY_RUNTIME,
        .message = message,
        .hint = strdup("Memory access outside permitted regions"),
        .location = (SourceLocation){0},
        .next = NULL
    };
    
    return error;
}