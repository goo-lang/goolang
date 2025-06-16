#ifndef IPNS_MANAGER_H
#define IPNS_MANAGER_H

#include "ipfs_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Forward declarations
typedef struct IpnsManager IpnsManager;
typedef struct IpnsKey IpnsKey;
typedef struct IpnsRecord IpnsRecord;
typedef struct IpnsSubscription IpnsSubscription;

// IPNS key management
typedef struct IpnsKey {
    char* name;                  // Key name (user-friendly)
    char* id;                    // Key ID (hash)
    char* public_key;           // Public key (base64)
    char* private_key;          // Private key (encrypted, base64)
    time_t created_at;          // Creation timestamp
    time_t last_used;           // Last usage timestamp
    bool is_default;            // Is this the default key
    struct IpnsKey* next;       // Linked list
} IpnsKey;

// IPNS record with metadata
typedef struct IpnsRecord {
    char* ipns_name;            // IPNS name (k51qzi5uqu5...)
    IpfsCid* current_cid;       // Current CID being pointed to
    char* value_path;           // Path within the CID (/path/to/content)
    
    // Record metadata
    time_t created_at;          // Record creation time
    time_t updated_at;          // Last update time
    time_t expires_at;          // Record expiration time
    uint64_t sequence_number;   // Sequence number for ordering
    
    // Resolution metadata
    time_t last_resolved;       // Last successful resolution
    time_t last_checked;        // Last resolution attempt
    bool is_resolvable;         // Can this record be resolved
    float resolution_confidence; // Confidence in resolution (0.0-1.0)
    
    // Caching and performance
    IpfsCid* cached_cid;        // Cached resolved CID
    time_t cache_expires;       // Cache expiration time
    int resolution_attempts;    // Number of resolution attempts
    float avg_resolution_time;  // Average resolution time (ms)
    
    // Publishing metadata
    IpnsKey* signing_key;       // Key used to sign this record
    bool auto_republish;        // Automatically republish when expired
    int republish_interval;     // Republish interval (seconds)
    
    struct IpnsRecord* next;    // Linked list
} IpnsRecord;

// IPNS subscription for real-time updates
typedef struct IpnsSubscription {
    char* ipns_name;            // IPNS name to monitor
    IpfsCid* last_known_cid;    // Last known CID
    
    // Callback for updates
    void (*update_callback)(const char* ipns_name, const IpfsCid* new_cid, void* user_data);
    void* user_data;            // User data for callback
    
    // Subscription configuration
    int check_interval;         // Check interval (seconds)
    bool notify_on_error;       // Notify on resolution errors
    bool cache_updates;         // Cache updates locally
    
    // Subscription state
    bool is_active;             // Is subscription active
    time_t last_check;          // Last check time
    time_t subscribed_at;       // Subscription start time
    int consecutive_failures;   // Consecutive resolution failures
    
    struct IpnsSubscription* next; // Linked list
} IpnsSubscription;

// IPNS Manager
typedef struct IpnsManager {
    IpfsClient* ipfs_client;
    
    // Key management
    IpnsKey* keys;              // Managed IPNS keys
    IpnsKey* default_key;       // Default publishing key
    char* keystore_path;        // Path to encrypted keystore
    
    // Record management
    IpnsRecord* records;        // Known IPNS records
    size_t record_count;
    
    // Subscriptions
    IpnsSubscription* subscriptions; // Active subscriptions
    bool subscription_thread_active; // Subscription monitoring thread
    
    // Configuration
    int default_ttl;            // Default TTL for published records (seconds)
    int resolution_timeout;     // Resolution timeout (seconds)
    int cache_duration;         // Cache duration (seconds)
    bool auto_republish_enabled; // Enable automatic republishing
    bool background_resolution; // Resolve records in background
    
    // Performance optimization
    size_t max_cache_size;      // Maximum cache size
    bool enable_prediction;     // Enable resolution prediction
    bool batch_operations;      // Batch multiple operations
} IpnsManager;

// Function declarations

// IPNS Manager lifecycle
IpnsManager* ipns_manager_create(IpfsClient* ipfs_client);
void ipns_manager_free(IpnsManager* manager);
bool ipns_manager_initialize(IpnsManager* manager);
bool ipns_manager_load_keystore(IpnsManager* manager, const char* keystore_path);
bool ipns_manager_save_keystore(IpnsManager* manager, const char* keystore_path);

// Key management
IpnsKey* ipns_manager_create_key(IpnsManager* manager, const char* key_name);
bool ipns_manager_import_key(IpnsManager* manager, const char* key_name, 
                           const char* private_key);
bool ipns_manager_export_key(IpnsManager* manager, const char* key_name, 
                           char** private_key_out);
bool ipns_manager_delete_key(IpnsManager* manager, const char* key_name);
IpnsKey** ipns_manager_list_keys(IpnsManager* manager, size_t* key_count);
IpnsKey* ipns_manager_get_key(IpnsManager* manager, const char* key_name);
bool ipns_manager_set_default_key(IpnsManager* manager, const char* key_name);

// Publishing operations
char* ipns_manager_publish(IpnsManager* manager, const IpfsCid* cid, 
                         const char* key_name);
char* ipns_manager_publish_with_path(IpnsManager* manager, const IpfsCid* cid, 
                                    const char* path, const char* key_name);
bool ipns_manager_republish(IpnsManager* manager, const char* ipns_name);
bool ipns_manager_update_record(IpnsManager* manager, const char* ipns_name, 
                              const IpfsCid* new_cid);

// Resolution operations
IpfsCid* ipns_manager_resolve(IpnsManager* manager, const char* ipns_name);
IpfsCid* ipns_manager_resolve_with_timeout(IpnsManager* manager, const char* ipns_name, 
                                         int timeout_seconds);
bool ipns_manager_resolve_async(IpnsManager* manager, const char* ipns_name,
                              void (*callback)(const char* ipns_name, const IpfsCid* cid, void* user_data),
                              void* user_data);

// Record management
IpnsRecord* ipns_manager_get_record(IpnsManager* manager, const char* ipns_name);
bool ipns_manager_add_record(IpnsManager* manager, IpnsRecord* record);
bool ipns_manager_remove_record(IpnsManager* manager, const char* ipns_name);
IpnsRecord** ipns_manager_list_records(IpnsManager* manager, size_t* record_count);
bool ipns_manager_refresh_record(IpnsManager* manager, const char* ipns_name);

// Subscription management
IpnsSubscription* ipns_manager_subscribe(IpnsManager* manager, const char* ipns_name,
                                       void (*callback)(const char* ipns_name, const IpfsCid* new_cid, void* user_data),
                                       void* user_data);
bool ipns_manager_unsubscribe(IpnsManager* manager, const char* ipns_name);
IpnsSubscription** ipns_manager_list_subscriptions(IpnsManager* manager, size_t* subscription_count);
bool ipns_manager_start_subscription_monitor(IpnsManager* manager);
bool ipns_manager_stop_subscription_monitor(IpnsManager* manager);

// Cache management
bool ipns_manager_cache_record(IpnsManager* manager, const char* ipns_name, 
                             const IpfsCid* cid, int ttl_seconds);
IpfsCid* ipns_manager_get_cached_record(IpnsManager* manager, const char* ipns_name);
bool ipns_manager_invalidate_cache(IpnsManager* manager, const char* ipns_name);
bool ipns_manager_clear_cache(IpnsManager* manager);
size_t ipns_manager_get_cache_size(IpnsManager* manager);

// Validation and verification
bool ipns_manager_validate_record(IpnsManager* manager, const char* ipns_name);
bool ipns_manager_verify_signature(IpnsManager* manager, const char* ipns_name);
bool ipns_manager_check_freshness(IpnsManager* manager, const char* ipns_name);

// Key operations
IpnsKey* ipns_key_create(const char* name);
void ipns_key_free(IpnsKey* key);
bool ipns_key_generate_keypair(IpnsKey* key);
bool ipns_key_load_from_file(IpnsKey* key, const char* filepath);
bool ipns_key_save_to_file(const IpnsKey* key, const char* filepath);
char* ipns_key_get_public_key_hash(const IpnsKey* key);

// Record operations
IpnsRecord* ipns_record_create(const char* ipns_name, const IpfsCid* cid);
void ipns_record_free(IpnsRecord* record);
bool ipns_record_update_cid(IpnsRecord* record, const IpfsCid* new_cid);
bool ipns_record_is_expired(const IpnsRecord* record);
bool ipns_record_needs_republish(const IpnsRecord* record);
float ipns_record_get_freshness_score(const IpnsRecord* record);

// Subscription operations
IpnsSubscription* ipns_subscription_create(const char* ipns_name);
void ipns_subscription_free(IpnsSubscription* subscription);
bool ipns_subscription_check_update(IpnsSubscription* subscription, IpnsManager* manager);
bool ipns_subscription_notify_update(IpnsSubscription* subscription, const IpfsCid* new_cid);

// Advanced features

// IPNS name aliasing (human-readable names)
typedef struct IpnsAlias {
    char* alias;                // Human-readable alias
    char* ipns_name;           // Actual IPNS name
    char* description;         // Description of the alias
    time_t created_at;         // Creation time
    struct IpnsAlias* next;    // Linked list
} IpnsAlias;

bool ipns_manager_add_alias(IpnsManager* manager, const char* alias, const char* ipns_name);
bool ipns_manager_remove_alias(IpnsManager* manager, const char* alias);
char* ipns_manager_resolve_alias(IpnsManager* manager, const char* alias);
IpnsAlias** ipns_manager_list_aliases(IpnsManager* manager, size_t* alias_count);

// IPNS record versioning
typedef struct IpnsVersion {
    uint64_t sequence_number;
    IpfsCid* cid;
    time_t timestamp;
    char* commit_message;      // Optional commit message
    struct IpnsVersion* next;
} IpnsVersion;

bool ipns_manager_enable_versioning(IpnsManager* manager, const char* ipns_name);
IpnsVersion** ipns_manager_get_version_history(IpnsManager* manager, const char* ipns_name, size_t* version_count);
bool ipns_manager_rollback_to_version(IpnsManager* manager, const char* ipns_name, uint64_t sequence_number);

// IPNS record sharing and collaboration
typedef struct IpnsCollaborator {
    char* public_key_hash;     // Collaborator's public key hash
    char* permissions;         // Permissions (read, write, admin)
    time_t added_at;          // When collaborator was added
    struct IpnsCollaborator* next;
} IpnsCollaborator;

bool ipns_manager_add_collaborator(IpnsManager* manager, const char* ipns_name, 
                                 const char* collaborator_key, const char* permissions);
bool ipns_manager_remove_collaborator(IpnsManager* manager, const char* ipns_name, 
                                    const char* collaborator_key);
IpnsCollaborator** ipns_manager_list_collaborators(IpnsManager* manager, const char* ipns_name, size_t* collaborator_count);

// Performance monitoring and optimization
typedef struct IpnsPerformanceStats {
    char* ipns_name;
    float avg_resolution_time;  // Average resolution time (ms)
    float success_rate;         // Resolution success rate (0.0-1.0)
    int total_resolutions;      // Total resolution attempts
    int cache_hits;            // Cache hit count
    int cache_misses;          // Cache miss count
    time_t stats_period_start; // Statistics period start
} IpnsPerformanceStats;

IpnsPerformanceStats** ipns_manager_get_performance_stats(IpnsManager* manager, size_t* stats_count);
void ipns_performance_stats_free(IpnsPerformanceStats* stats);
bool ipns_manager_optimize_resolution(IpnsManager* manager);

// Configuration management
typedef struct IpnsManagerConfig {
    int default_ttl;
    int resolution_timeout;
    int cache_duration;
    bool auto_republish_enabled;
    bool background_resolution;
    size_t max_cache_size;
    bool enable_prediction;
    bool batch_operations;
    char* keystore_path;
} IpnsManagerConfig;

IpnsManagerConfig* ipns_manager_config_create_default(void);
IpnsManagerConfig* ipns_manager_config_load(const char* config_file);
bool ipns_manager_config_save(const IpnsManagerConfig* config, const char* config_file);
void ipns_manager_config_free(IpnsManagerConfig* config);

// Integration with package management
bool ipns_manager_register_package(IpnsManager* manager, const char* package_name, 
                                  const char* ipns_name);
char* ipns_manager_resolve_package(IpnsManager* manager, const char* package_name);
bool ipns_manager_update_package(IpnsManager* manager, const char* package_name, 
                                const IpfsCid* new_cid);

// ENS (Ethereum Name Service) integration
bool ipns_manager_resolve_ens(IpnsManager* manager, const char* ens_name, char** ipns_name_out);
bool ipns_manager_register_ens(IpnsManager* manager, const char* ens_name, const char* ipns_name);

#endif // IPNS_MANAGER_H