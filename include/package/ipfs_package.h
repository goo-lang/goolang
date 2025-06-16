#ifndef IPFS_PACKAGE_H
#define IPFS_PACKAGE_H

#include "ipfs_client.h"
#include "goo_mod.h"
#include "package_manager.h"

// Forward declarations
typedef struct IpfsPackageManager IpfsPackageManager;
typedef struct IpfsPackageCache IpfsPackageCache;
typedef struct IpfsResolution IpfsResolution;

// IPFS-enhanced package resolution
typedef struct IpfsResolution {
    char* package_name;
    Version* version;
    IpfsCid* ipfs_cid;           // IPFS content ID
    char* ipns_name;             // IPNS mutable reference
    char* registry_url;          // Fallback registry URL
    PackageDistribution* distribution;
    float resolution_confidence; // AI confidence score
    char* resolution_source;     // How this was resolved
    time_t resolved_at;
} IpfsResolution;

// IPFS package cache entry
typedef struct IpfsCacheEntry {
    IpfsCid* cid;
    char* package_name;
    Version* version;
    char* local_path;
    size_t size;
    time_t cached_at;
    time_t last_accessed;
    bool is_pinned;
    int access_count;
    struct IpfsCacheEntry* next;
} IpfsCacheEntry;

// IPFS package cache
typedef struct IpfsPackageCache {
    IpfsCacheEntry* entries;
    char* cache_dir;
    size_t total_size;
    size_t max_size;
    size_t entry_count;
    size_t max_entries;
} IpfsPackageCache;

// IPFS-enabled package manager
typedef struct IpfsPackageManager {
    PackageManager* base_manager;
    IpfsClient* ipfs_client;
    IpfsPackageCache* cache;
    bool hybrid_mode;            // Use both IPFS and traditional
    bool prefer_ipfs;            // Prefer IPFS when available
    bool auto_pin_dependencies;  // Auto-pin important deps
    char* swarm_name;           // P2P swarm to join
} IpfsPackageManager;

// Enhanced Goo module with IPFS support
typedef struct GooModIpfs {
    GooMod* base_module;
    
    // IPFS-specific fields
    IpfsCid* ipfs_cid;           // Package content CID
    char* ipns_name;             // IPNS mutable reference
    PackageDistribution* distribution;
    
    // IPFS metadata
    char** ipfs_gateways;        // Preferred gateways
    size_t gateway_count;
    bool pin_locally;
    bool announce_to_swarm;
    
    // P2P configuration
    char* swarm_topic;
    bool enable_p2p_discovery;
    bool share_bandwidth;
    
    // Performance settings
    int max_concurrent_downloads;
    int gateway_timeout;
    size_t cache_size_limit;
} GooModIpfs;

// Function declarations

// IPFS Package Manager lifecycle
IpfsPackageManager* ipfs_package_manager_create(const char* workspace_root, 
                                               const IpfsClientConfig* ipfs_config);
void ipfs_package_manager_free(IpfsPackageManager* manager);
bool ipfs_package_manager_initialize(IpfsPackageManager* manager);

// Package resolution with IPFS support
IpfsResolution* ipfs_resolve_package(IpfsPackageManager* manager, 
                                   const char* package_name, 
                                   const char* version_constraint);
IpfsResolution** ipfs_resolve_dependencies(IpfsPackageManager* manager, 
                                         const GooModIpfs* module,
                                         size_t* resolution_count);
void ipfs_resolution_free(IpfsResolution* resolution);

// Package installation with IPFS
bool ipfs_install_package(IpfsPackageManager* manager, const IpfsResolution* resolution);
bool ipfs_install_from_cid(IpfsPackageManager* manager, const IpfsCid* cid, const char* package_name);
bool ipfs_install_from_ipns(IpfsPackageManager* manager, const char* ipns_name, const char* package_name);
bool ipfs_install_all_dependencies(IpfsPackageManager* manager, const GooModIpfs* module);

// Package publishing to IPFS
IpfsCid* ipfs_publish_package(IpfsPackageManager* manager, const char* package_dir);
char* ipfs_publish_to_ipns(IpfsPackageManager* manager, const IpfsCid* cid, const char* key_name);
bool ipfs_announce_package(IpfsPackageManager* manager, const IpfsCid* cid, const char* swarm_topic);

// Hybrid resolution (IPFS + traditional registry)
typedef enum {
    HYBRID_STRATEGY_IPFS_FIRST,     // Try IPFS first, fallback to registry
    HYBRID_STRATEGY_REGISTRY_FIRST, // Try registry first, fallback to IPFS
    HYBRID_STRATEGY_FASTEST,        // Use whichever responds first
    HYBRID_STRATEGY_MOST_RECENT,    // Use most recently updated
    HYBRID_STRATEGY_MOST_RELIABLE   // Use most reliable source
} HybridResolutionStrategy;

IpfsResolution* ipfs_hybrid_resolve(IpfsPackageManager* manager,
                                  const char* package_name,
                                  const char* version_constraint,
                                  HybridResolutionStrategy strategy);

// Cache management
IpfsPackageCache* ipfs_cache_create(const char* cache_dir, size_t max_size);
void ipfs_cache_free(IpfsPackageCache* cache);
bool ipfs_cache_add(IpfsPackageCache* cache, const IpfsCid* cid, 
                   const char* package_name, const Version* version, const char* local_path);
IpfsCacheEntry* ipfs_cache_find(IpfsPackageCache* cache, const IpfsCid* cid);
IpfsCacheEntry* ipfs_cache_find_by_name(IpfsPackageCache* cache, 
                                       const char* package_name, const Version* version);
bool ipfs_cache_remove(IpfsPackageCache* cache, const IpfsCid* cid);
bool ipfs_cache_cleanup(IpfsPackageCache* cache); // Remove old/unused entries
size_t ipfs_cache_get_size(IpfsPackageCache* cache);

// Enhanced goo.mod with IPFS support
GooModIpfs* goo_mod_ipfs_create(const char* module_path, const char* version);
void goo_mod_ipfs_free(GooModIpfs* module);
GooModIpfs* goo_mod_ipfs_parse_file(const char* filepath);
GooModIpfs* goo_mod_ipfs_parse_string(const char* content);
bool goo_mod_ipfs_save_file(const GooModIpfs* module, const char* filepath);
char* goo_mod_ipfs_to_string(const GooModIpfs* module);

// IPFS-specific dependency operations
bool goo_mod_ipfs_add_dependency_cid(GooModIpfs* module, const char* name, const IpfsCid* cid);
bool goo_mod_ipfs_add_dependency_ipns(GooModIpfs* module, const char* name, const char* ipns_name);
bool goo_mod_ipfs_set_distribution(GooModIpfs* module, PackageDistribution* distribution);

// P2P discovery and sharing
typedef struct IpfsDiscoveryResult {
    char* package_name;
    IpfsCid* cid;
    Version* version;
    char* publisher_peer_id;
    time_t published_at;
    int peer_count;              // Number of peers sharing this
    float reputation_score;      // Reputation of publisher
} IpfsDiscoveryResult;

IpfsDiscoveryResult** ipfs_discover_packages(IpfsPackageManager* manager, 
                                           const char* search_query,
                                           size_t* result_count);
bool ipfs_share_local_packages(IpfsPackageManager* manager);
void ipfs_discovery_result_free(IpfsDiscoveryResult* result);

// Content verification and security
typedef struct IpfsSecurityCheck {
    bool content_verified;       // Content hash matches CID
    bool signatures_verified;    // Cryptographic signatures valid
    bool reputation_acceptable;  // Publisher reputation OK
    bool no_vulnerabilities;     // No known vulnerabilities
    char** warning_messages;     // Security warnings
    size_t warning_count;
    char** error_messages;       // Security errors
    size_t error_count;
} IpfsSecurityCheck;

IpfsSecurityCheck* ipfs_verify_package_security(IpfsPackageManager* manager, 
                                               const IpfsResolution* resolution);
void ipfs_security_check_free(IpfsSecurityCheck* check);

// Performance optimization
typedef struct IpfsPerformanceProfile {
    char* gateway_url;
    float avg_response_time;     // Average response time (ms)
    float reliability_score;     // Reliability (0.0-1.0)
    size_t bytes_transferred;
    size_t successful_requests;
    size_t failed_requests;
    time_t last_updated;
} IpfsPerformanceProfile;

IpfsPerformanceProfile** ipfs_get_gateway_performance(IpfsPackageManager* manager, size_t* count);
bool ipfs_optimize_gateway_selection(IpfsPackageManager* manager);
void ipfs_performance_profile_free(IpfsPerformanceProfile* profile);

// Parallel downloads and streaming
typedef struct IpfsDownloadJob {
    IpfsCid* cid;
    char* output_path;
    IpfsProgressCallback progress_callback;
    void* user_data;
    bool completed;
    bool success;
    char* error_message;
} IpfsDownloadJob;

bool ipfs_download_parallel(IpfsPackageManager* manager, 
                           IpfsDownloadJob** jobs, 
                           size_t job_count);
bool ipfs_stream_and_compile(IpfsPackageManager* manager, 
                            const IpfsResolution** resolutions,
                            size_t resolution_count);

// Gateway management and health monitoring
typedef struct IpfsGatewayHealth {
    char* gateway_url;
    bool is_responsive;
    float response_time;         // ms
    bool supports_required_features;
    time_t last_check;
    char* error_message;
} IpfsGatewayHealth;

IpfsGatewayHealth** ipfs_check_gateway_health(IpfsPackageManager* manager, size_t* count);
bool ipfs_auto_configure_gateways(IpfsPackageManager* manager);
void ipfs_gateway_health_free(IpfsGatewayHealth* health);

// Statistics and monitoring
typedef struct IpfsPackageStats {
    size_t packages_cached;
    size_t packages_pinned;
    size_t total_cache_size;
    size_t ipfs_downloads;
    size_t registry_downloads;
    size_t p2p_shares;
    float ipfs_success_rate;
    float avg_ipfs_speed;        // MB/s
    float avg_registry_speed;    // MB/s
    time_t session_start;
} IpfsPackageStats;

IpfsPackageStats* ipfs_get_package_stats(IpfsPackageManager* manager);
void ipfs_reset_package_stats(IpfsPackageManager* manager);
void ipfs_package_stats_free(IpfsPackageStats* stats);

// CLI integration helpers
bool ipfs_cmd_publish(IpfsPackageManager* manager, const char* package_dir, bool announce);
bool ipfs_cmd_pin(IpfsPackageManager* manager, const char* cid_or_name);
bool ipfs_cmd_unpin(IpfsPackageManager* manager, const char* cid_or_name);
bool ipfs_cmd_share(IpfsPackageManager* manager, const char* swarm_name);
bool ipfs_cmd_discover(IpfsPackageManager* manager, const char* search_query);
bool ipfs_cmd_gateway_add(IpfsPackageManager* manager, const char* gateway_url);
bool ipfs_cmd_gateway_check(IpfsPackageManager* manager);

// Configuration helpers
typedef struct IpfsPackageConfig {
    bool enable_ipfs;
    bool prefer_ipfs;
    bool auto_pin_dependencies;
    bool enable_p2p_sharing;
    char* default_swarm;
    char** preferred_gateways;
    size_t gateway_count;
    size_t cache_size_limit;
    int gateway_timeout;
    HybridResolutionStrategy hybrid_strategy;
} IpfsPackageConfig;

IpfsPackageConfig* ipfs_package_config_create_default(void);
IpfsPackageConfig* ipfs_package_config_load(const char* config_path);
bool ipfs_package_config_save(const IpfsPackageConfig* config, const char* config_path);
void ipfs_package_config_free(IpfsPackageConfig* config);

#endif // IPFS_PACKAGE_H