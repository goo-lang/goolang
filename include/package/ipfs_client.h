#ifndef IPFS_CLIENT_H
#define IPFS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Forward declarations
typedef struct IpfsClient IpfsClient;
typedef struct IpfsNode IpfsNode;
typedef struct IpfsCid IpfsCid;
typedef struct IpfsGateway IpfsGateway;
typedef struct IpfsSwarm IpfsSwarm;

// IPFS Content Identifier (CID)
typedef struct IpfsCid {
    char* hash;                    // Base58 encoded hash
    char* multihash;              // Raw multihash
    size_t size;                  // Content size in bytes
    char* codec;                  // Content codec (dag-pb, raw, etc.)
    int version;                  // CID version (0 or 1)
} IpfsCid;

// IPFS Gateway configuration
typedef struct IpfsGateway {
    char* url;                    // Gateway URL
    char* name;                   // Gateway name
    bool is_local;                // Is this a local node?
    float response_time;          // Average response time (ms)
    float reliability;            // Reliability score (0.0-1.0)
    bool supports_car;            // Supports CAR format
    bool supports_subdomain;      // Supports subdomain requests
    time_t last_check;            // Last health check
    struct IpfsGateway* next;     // Linked list
} IpfsGateway;

// IPFS Node information
typedef struct IpfsNode {
    char* peer_id;                // Node peer ID
    char* api_endpoint;           // API endpoint URL
    char* gateway_endpoint;       // Gateway endpoint URL
    bool is_local;                // Is this the local node?
    bool is_online;               // Node status
    char* version;                // IPFS version
    size_t storage_used;          // Storage used (bytes)
    size_t storage_limit;         // Storage limit (bytes)
    struct IpfsNode* next;        // Linked list for multiple nodes
} IpfsNode;

// IPFS Swarm configuration
typedef struct IpfsSwarm {
    char* name;                   // Swarm name
    char* topic;                  // PubSub topic
    char** peers;                 // Connected peers
    size_t peer_count;
    bool auto_announce;           // Auto-announce packages
    bool auto_discover;           // Auto-discover packages
} IpfsSwarm;

// Package distribution configuration
typedef enum {
    DIST_IPFS_ONLY,              // IPFS only
    DIST_REGISTRY_ONLY,          // Traditional registry only
    DIST_HYBRID,                 // Both IPFS and registry
    DIST_AUTO                    // Automatic selection
} DistributionMode;

typedef struct PackageDistribution {
    DistributionMode mode;
    IpfsCid* ipfs_cid;           // IPFS content ID
    char* ipns_name;             // IPNS name (mutable reference)
    char* registry_url;          // Traditional registry URL
    bool pin_locally;            // Pin package locally
    bool announce_to_swarm;      // Announce to P2P swarm
    char** preferred_gateways;   // Preferred gateways
    size_t gateway_count;
} PackageDistribution;

// IPFS Client configuration
typedef struct IpfsClientConfig {
    IpfsNode* local_node;        // Local IPFS node
    IpfsGateway* gateways;       // Available gateways
    IpfsSwarm* swarms;           // P2P swarms
    char* cache_dir;             // Local cache directory
    size_t cache_size_limit;     // Cache size limit
    int gateway_timeout;         // Gateway timeout (seconds)
    int max_concurrent_downloads; // Max parallel downloads
    bool auto_start_daemon;      // Auto-start IPFS daemon
    bool enable_p2p_sharing;     // Enable P2P sharing
    bool verify_content;         // Verify content integrity
} IpfsClientConfig;

// IPFS Client instance
typedef struct IpfsClient {
    IpfsClientConfig* config;
    IpfsNode* current_node;      // Currently active node
    IpfsGateway* active_gateway; // Currently active gateway
    char* cache_dir;             // Cache directory
    bool is_daemon_running;      // IPFS daemon status
    void* curl_handle;           // HTTP client handle
} IpfsClient;

// Download progress callback
typedef void (*IpfsProgressCallback)(size_t downloaded, size_t total, void* user_data);

// Content verification result
typedef struct IpfsVerification {
    bool is_valid;
    char* expected_hash;
    char* actual_hash;
    size_t expected_size;
    size_t actual_size;
    char* error_message;
} IpfsVerification;

// Function declarations

// Client lifecycle
IpfsClient* ipfs_client_create(const IpfsClientConfig* config);
void ipfs_client_free(IpfsClient* client);
bool ipfs_client_initialize(IpfsClient* client);
bool ipfs_client_shutdown(IpfsClient* client);

// IPFS daemon management
bool ipfs_daemon_start(IpfsClient* client);
bool ipfs_daemon_stop(IpfsClient* client);
bool ipfs_daemon_is_running(IpfsClient* client);
bool ipfs_daemon_restart(IpfsClient* client);

// Content operations
IpfsCid* ipfs_add_file(IpfsClient* client, const char* filepath);
IpfsCid* ipfs_add_directory(IpfsClient* client, const char* dirpath);
IpfsCid* ipfs_add_data(IpfsClient* client, const void* data, size_t size);
bool ipfs_get_file(IpfsClient* client, const IpfsCid* cid, const char* output_path);
bool ipfs_get_directory(IpfsClient* client, const IpfsCid* cid, const char* output_dir);
char* ipfs_cat(IpfsClient* client, const IpfsCid* cid, size_t* size);

// Content with progress callbacks
bool ipfs_get_file_progress(IpfsClient* client, const IpfsCid* cid, 
                           const char* output_path, IpfsProgressCallback callback, void* user_data);

// Content verification
IpfsVerification* ipfs_verify_content(IpfsClient* client, const char* filepath, const IpfsCid* expected_cid);
void ipfs_verification_free(IpfsVerification* verification);

// Pinning operations
bool ipfs_pin_add(IpfsClient* client, const IpfsCid* cid);
bool ipfs_pin_remove(IpfsClient* client, const IpfsCid* cid);
bool ipfs_pin_list(IpfsClient* client, IpfsCid*** pinned_cids, size_t* count);
bool ipfs_pin_verify(IpfsClient* client, const IpfsCid* cid);

// IPNS operations
char* ipfs_ipns_publish(IpfsClient* client, const IpfsCid* cid, const char* key_name);
IpfsCid* ipfs_ipns_resolve(IpfsClient* client, const char* ipns_name);
bool ipfs_ipns_republish(IpfsClient* client, const char* ipns_name);

// Gateway operations
IpfsGateway* ipfs_gateway_create(const char* url, const char* name);
void ipfs_gateway_free(IpfsGateway* gateway);
bool ipfs_gateway_health_check(IpfsClient* client, IpfsGateway* gateway);
IpfsGateway* ipfs_gateway_select_best(IpfsClient* client);
bool ipfs_gateway_add(IpfsClient* client, IpfsGateway* gateway);
bool ipfs_gateway_remove(IpfsClient* client, const char* url);

// Node operations
IpfsNode* ipfs_node_create(const char* peer_id, const char* api_endpoint);
void ipfs_node_free(IpfsNode* node);
bool ipfs_node_connect(IpfsClient* client, IpfsNode* node);
bool ipfs_node_disconnect(IpfsClient* client, IpfsNode* node);
bool ipfs_node_get_info(IpfsClient* client, IpfsNode* node);

// Swarm operations
IpfsSwarm* ipfs_swarm_create(const char* name, const char* topic);
void ipfs_swarm_free(IpfsSwarm* swarm);
bool ipfs_swarm_join(IpfsClient* client, IpfsSwarm* swarm);
bool ipfs_swarm_leave(IpfsClient* client, IpfsSwarm* swarm);
bool ipfs_swarm_announce(IpfsClient* client, IpfsSwarm* swarm, const IpfsCid* cid);
bool ipfs_swarm_discover(IpfsClient* client, IpfsSwarm* swarm, IpfsCid*** discovered_cids, size_t* count);

// CID operations
IpfsCid* ipfs_cid_create(const char* hash);
void ipfs_cid_free(IpfsCid* cid);
IpfsCid* ipfs_cid_parse(const char* cid_string);
char* ipfs_cid_to_string(const IpfsCid* cid);
bool ipfs_cid_validate(const char* cid_string);
bool ipfs_cid_equals(const IpfsCid* a, const IpfsCid* b);

// Package-specific operations
typedef struct IpfsPackageManifest {
    char* name;
    char* version;
    IpfsCid* content_cid;
    IpfsCid* source_cid;
    IpfsCid* docs_cid;
    IpfsCid* tests_cid;
    char** signatures;           // Cryptographic signatures
    size_t signature_count;
    time_t published_at;
    char* publisher_peer_id;
    size_t total_size;
    int pin_count;               // Number of nodes pinning this
} IpfsPackageManifest;

IpfsPackageManifest* ipfs_package_publish(IpfsClient* client, const char* package_dir);
IpfsPackageManifest* ipfs_package_get_manifest(IpfsClient* client, const IpfsCid* cid);
bool ipfs_package_download(IpfsClient* client, const IpfsPackageManifest* manifest, const char* output_dir);
bool ipfs_package_verify_signatures(IpfsClient* client, const IpfsPackageManifest* manifest);
void ipfs_package_manifest_free(IpfsPackageManifest* manifest);

// Performance monitoring
typedef struct IpfsStats {
    size_t bytes_downloaded;
    size_t bytes_uploaded;
    size_t files_cached;
    size_t cache_hits;
    size_t cache_misses;
    float avg_download_speed;    // MB/s
    float avg_upload_speed;      // MB/s
    int active_connections;
    time_t session_start;
} IpfsStats;

IpfsStats* ipfs_get_stats(IpfsClient* client);
void ipfs_stats_reset(IpfsClient* client);
void ipfs_stats_free(IpfsStats* stats);

// Configuration management
IpfsClientConfig* ipfs_config_create_default(void);
IpfsClientConfig* ipfs_config_load_from_file(const char* config_path);
bool ipfs_config_save_to_file(const IpfsClientConfig* config, const char* config_path);
void ipfs_config_free(IpfsClientConfig* config);

// Utility functions
bool ipfs_is_valid_hash(const char* hash);
char* ipfs_hash_file(const char* filepath);
char* ipfs_hash_data(const void* data, size_t size);
bool ipfs_ensure_directory(const char* path);
char* ipfs_get_cache_path(IpfsClient* client, const IpfsCid* cid);

// Error handling
typedef enum {
    IPFS_SUCCESS = 0,
    IPFS_ERROR_INVALID_CID,
    IPFS_ERROR_NETWORK,
    IPFS_ERROR_TIMEOUT,
    IPFS_ERROR_NOT_FOUND,
    IPFS_ERROR_PERMISSION_DENIED,
    IPFS_ERROR_INSUFFICIENT_STORAGE,
    IPFS_ERROR_DAEMON_NOT_RUNNING,
    IPFS_ERROR_INVALID_CONFIG,
    IPFS_ERROR_VERIFICATION_FAILED,
    IPFS_ERROR_GATEWAY_UNAVAILABLE
} IpfsError;

const char* ipfs_error_string(IpfsError error);
IpfsError ipfs_get_last_error(IpfsClient* client);
void ipfs_set_error(IpfsClient* client, IpfsError error, const char* message);

#endif // IPFS_CLIENT_H