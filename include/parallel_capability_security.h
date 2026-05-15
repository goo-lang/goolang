#ifndef PARALLEL_CAPABILITY_SECURITY_H
#define PARALLEL_CAPABILITY_SECURITY_H

#include <stdint.h>
#include <stdbool.h>
#include "ccomp_shim.h"
#include "structured_concurrency.h"
#include "work_stealing.h"

// Forward declarations
typedef struct SecurityCapability SecurityCapability;
typedef struct CapabilityToken CapabilityToken;
typedef struct SecureTaskContext SecureTaskContext;

// Define ParallelForTaskArgs here since it's needed for secure tasks
typedef struct ParallelForTaskArgs {
    size_t start;
    size_t end;
    ParallelForFunction func;
    void* user_context;
} ParallelForTaskArgs;

// Security capability types for parallel operations
typedef enum CapabilityType {
    CAP_MEMORY_READ = 1 << 0,       // Read access to memory regions
    CAP_MEMORY_WRITE = 1 << 1,      // Write access to memory regions
    CAP_FILE_READ = 1 << 2,         // Read file system access
    CAP_FILE_WRITE = 1 << 3,        // Write file system access
    CAP_NETWORK_READ = 1 << 4,      // Network read operations
    CAP_NETWORK_WRITE = 1 << 5,     // Network write operations
    CAP_SYSTEM_CALL = 1 << 6,       // System call execution
    CAP_THREAD_CREATE = 1 << 7,     // Thread creation
    CAP_TIMER_ACCESS = 1 << 8,      // Timer and time access
    CAP_IPC_SEND = 1 << 9,          // Inter-process communication send
    CAP_IPC_RECEIVE = 1 << 10,      // Inter-process communication receive
    CAP_DEVICE_ACCESS = 1 << 11,    // Hardware device access
    CAP_DEBUG_ACCESS = 1 << 12,     // Debugging capabilities
    CAP_CRYPTO_OPERATIONS = 1 << 13, // Cryptographic operations
    CAP_ALL = 0x3FFF                // All capabilities (for testing)
} CapabilityType;

// Security levels for parallel tasks
typedef enum SecurityLevel {
    SECURITY_LEVEL_UNTRUSTED = 0,   // No capabilities by default
    SECURITY_LEVEL_RESTRICTED,      // Limited capabilities
    SECURITY_LEVEL_STANDARD,        // Standard application capabilities
    SECURITY_LEVEL_PRIVILEGED,      // Extended capabilities
    SECURITY_LEVEL_SYSTEM          // Full system access
} SecurityLevel;

// Memory region descriptor for capability-based access
typedef struct MemoryRegion {
    void* base_address;
    size_t size;
    uint32_t access_flags;          // CapabilityType flags
    char* region_name;
    uint64_t region_id;
    atomic_uint_fast32_t access_count;
} MemoryRegion;

// Security capability structure
typedef struct SecurityCapability {
    uint64_t capability_id;
    CapabilityType type;
    SecurityLevel required_level;
    
    // Resource constraints
    void* resource_ptr;             // Pointer to resource (memory, file, etc.)
    size_t resource_size;
    uint64_t expiry_time;           // Capability expiration (0 = never)
    
    // Access limits
    uint32_t max_accesses;          // Maximum number of accesses (0 = unlimited)
    atomic_uint_fast32_t access_count;
    
    // Delegation info
    uint64_t delegated_from;        // Original capability ID (0 = root)
    bool can_delegate;              // Can this capability be further delegated
    
    // Validation
    bool is_valid;
    uint64_t creation_time;
    char* description;
} SecurityCapability;

// Capability token for task execution
typedef struct CapabilityToken {
    uint64_t token_id;
    SecurityLevel security_level;
    
    // Granted capabilities
    SecurityCapability** capabilities;
    size_t capability_count;
    size_t capability_capacity;
    
    // Access tracking
    atomic_uint_fast64_t total_accesses;
    atomic_uint_fast32_t violations;
    
    // Token metadata
    uint64_t task_id;
    uint64_t worker_id;
    bool is_active;
    uint64_t creation_time;
    uint64_t last_access_time;
} CapabilityToken;

// Secure task context with capability enforcement
typedef struct SecureTaskContext {
    TaskContext base_context;
    
    // Security context
    CapabilityToken* token;
    SecurityLevel task_security_level;
    
    // Resource tracking
    MemoryRegion** accessible_regions;
    size_t region_count;
    
    // Violation handling
    void (*on_violation)(uint64_t task_id, CapabilityType attempted_cap, const char* details);
    bool strict_enforcement;
    bool audit_all_accesses;
    
    // Performance impact
    atomic_uint_fast32_t security_checks;
    atomic_uint_fast64_t security_check_time_ns;
} SecureTaskContext;

// Configuration for capability-based parallel execution
typedef struct CapabilitySecureParallelConfig {
    ParallelForConfig base_config;
    
    // Security settings
    SecurityLevel default_security_level;
    bool enable_capability_checking;
    bool enable_access_auditing;
    bool strict_isolation;
    
    // Resource permissions
    MemoryRegion** allowed_memory_regions;
    size_t memory_region_count;
    
    // Capability templates
    CapabilityType default_capabilities;
    SecurityCapability** granted_capabilities;
    size_t granted_capability_count;
    
    // Violation handling
    void (*on_security_violation)(uint64_t task_id, CapabilityType attempted_cap, const char* details);
    void (*on_access_denied)(uint64_t task_id, void* resource, const char* reason);
    bool terminate_on_violation;
    
    // Audit settings
    bool log_all_accesses;
    bool track_capability_usage;
    char* audit_log_path;
} CapabilitySecureParallelConfig;

// Capability management functions
CapabilityToken* capability_token_create(SecurityLevel level, uint64_t task_id);
void capability_token_destroy(CapabilityToken* token);
bool capability_token_grant(CapabilityToken* token, SecurityCapability* capability);
bool capability_token_revoke(CapabilityToken* token, uint64_t capability_id);
bool capability_token_has_capability(CapabilityToken* token, CapabilityType type, void* resource);

// Security capability creation and management
SecurityCapability* security_capability_create(CapabilityType type, SecurityLevel required_level, 
                                              void* resource, size_t resource_size);
void security_capability_destroy(SecurityCapability* capability);
bool security_capability_is_valid(SecurityCapability* capability);
SecurityCapability* security_capability_delegate(SecurityCapability* source, SecurityLevel target_level);

// Memory region management
MemoryRegion* memory_region_create(void* base, size_t size, uint32_t access_flags, const char* name);
void memory_region_destroy(MemoryRegion* region);
bool memory_region_check_access(MemoryRegion* region, void* address, size_t size, CapabilityType access_type);

// Access validation functions
bool validate_memory_access(SecureTaskContext* ctx, void* address, size_t size, CapabilityType access_type);
bool validate_capability_access(SecureTaskContext* ctx, CapabilityType required_cap, void* resource);
void record_security_violation(SecureTaskContext* ctx, CapabilityType attempted_cap, const char* details);
void audit_capability_access(SecureTaskContext* ctx, CapabilityType cap_type, bool granted);

// Secure parallel execution functions
Result_void_ptr capability_secure_parallel_for(
    TaskScope* scope,
    CapabilitySecureParallelConfig config,
    ParallelForFunction function,
    void* context
);

Result_void_ptr capability_secure_work_stealing_parallel_for(
    WorkStealingScope* scope,
    CapabilitySecureParallelConfig config,
    ParallelForFunction function,
    void* context
);

// Secure task wrapper
Result_void_ptr capability_secure_task_wrapper(TaskContext* task_ctx, void* args);

// Configuration presets
CapabilitySecureParallelConfig capability_secure_config_default(void);
CapabilitySecureParallelConfig capability_secure_config_strict(void);
CapabilitySecureParallelConfig capability_secure_config_untrusted(void);
CapabilitySecureParallelConfig capability_secure_config_debug(void);

// Utility functions
const char* capability_type_to_string(CapabilityType type);
const char* security_level_to_string(SecurityLevel level);
uint64_t get_capability_timestamp(void);
void print_capability_token(CapabilityToken* token);
void print_security_summary(SecureTaskContext* ctx);

// Predefined capability sets
SecurityCapability** create_read_only_capabilities(size_t* count);
SecurityCapability** create_compute_only_capabilities(size_t* count);
SecurityCapability** create_network_capabilities(size_t* count);
SecurityCapability** create_file_system_capabilities(size_t* count);

// Integration macros for easy capability checking
#define CHECK_CAPABILITY(ctx, cap_type, resource) \
    do { \
        if (!validate_capability_access(ctx, cap_type, resource)) { \
            record_security_violation(ctx, cap_type, "Capability check failed"); \
            return ERR_PTR(security_create_access_denied_error(cap_type, resource)); \
        } \
    } while(0)

#define CHECK_MEMORY_ACCESS(ctx, addr, size, access_type) \
    do { \
        if (!validate_memory_access(ctx, addr, size, access_type)) { \
            record_security_violation(ctx, access_type, "Memory access denied"); \
            return ERR_PTR(security_create_memory_violation_error(addr, size)); \
        } \
    } while(0)

#define AUDIT_ACCESS(ctx, cap_type, granted) \
    do { \
        if (ctx->audit_all_accesses) audit_capability_access(ctx, cap_type, granted); \
    } while(0)

// Error creation helpers
Error* security_create_access_denied_error(CapabilityType cap_type, void* resource);
Error* security_create_memory_violation_error(void* address, size_t size);
Error* security_create_capability_expired_error(uint64_t capability_id);

#endif // PARALLEL_CAPABILITY_SECURITY_H