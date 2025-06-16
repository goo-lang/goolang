#ifndef P2P_DISCOVERY_H
#define P2P_DISCOVERY_H

#include "ipfs_client.h"
#include "crypto_verifier.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Forward declarations
typedef struct P2PDiscovery P2PDiscovery;
typedef struct P2PPeer P2PPeer;
typedef struct P2PPackageAnnouncement P2PPackageAnnouncement;
typedef struct P2PSwarm P2PSwarm;
typedef struct P2PSearchQuery P2PSearchQuery;
typedef struct P2PSearchResult P2PSearchResult;

// Peer information
typedef struct P2PPeer {
    char* peer_id;                  // Unique peer identifier
    char* multiaddr;                // Multiaddress for connection
    char* agent_version;            // Goo version string
    
    // Peer capabilities
    bool supports_package_sharing;   // Can share packages
    bool supports_bandwidth_donation; // Willing to donate bandwidth
    bool supports_cache_sharing;     // Shares local cache
    bool supports_relay;            // Can relay for NAT traversal
    
    // Reputation and trust
    float reputation_score;         // Overall reputation (0.0-1.0)
    int packages_shared;            // Number of packages shared
    int packages_received;          // Number of packages received
    float upload_ratio;             // Upload/download ratio
    int malicious_reports;          // Reports of malicious behavior
    
    // Connection quality
    float latency_ms;               // Network latency
    float bandwidth_mbps;           // Available bandwidth
    time_t last_seen;               // Last successful interaction
    int consecutive_failures;       // Connection failures
    
    // Geographic information
    char* country_code;             // ISO country code
    char* region;                   // Geographic region
    float latitude;                 // Approximate latitude
    float longitude;                // Approximate longitude
    
    struct P2PPeer* next;           // Linked list
} P2PPeer;

// Package announcement for P2P sharing
typedef struct P2PPackageAnnouncement {
    char* package_name;             // Package name
    char* version;                  // Package version
    IpfsCid* cid;                   // Content identifier
    
    // Announcement metadata
    char* announcer_peer_id;        // Who announced this
    time_t announced_at;            // When it was announced
    time_t expires_at;              // Announcement expiration
    
    // Package metadata
    size_t package_size;            // Package size in bytes
    char* description;              // Brief description
    char** tags;                    // Searchable tags
    size_t tag_count;
    
    // Availability information
    P2PPeer** seeders;              // Peers who have this package
    size_t seeder_count;
    float availability_score;       // How available is this package
    
    // Verification
    PackageSignature** signatures;  // Package signatures
    size_t signature_count;
    bool is_verified;               // Has been verified
    
    struct P2PPackageAnnouncement* next;
} P2PPackageAnnouncement;

// P2P swarm for topic-based discovery
typedef struct P2PSwarm {
    char* topic;                    // Swarm topic (e.g., "goo-packages")
    char* description;              // Human-readable description
    
    // Swarm members
    P2PPeer** peers;                // Active peers in swarm
    size_t peer_count;
    size_t peer_capacity;
    
    // Swarm settings
    bool is_public;                 // Public vs private swarm
    char* access_key;               // Key for private swarms
    int max_peers;                  // Maximum peer limit
    
    // Activity tracking
    time_t created_at;              // When swarm was created
    time_t last_activity;           // Last message/announcement
    int message_count;              // Total messages exchanged
    
    struct P2PSwarm* next;          // Linked list
} P2PSwarm;

// Search query for package discovery
typedef struct P2PSearchQuery {
    char* query_string;             // Search query
    
    // Search filters
    char** tags;                    // Required tags
    size_t tag_count;
    char* min_version;              // Minimum version
    char* max_version;              // Maximum version
    size_t min_size;                // Minimum package size
    size_t max_size;                // Maximum package size
    
    // Quality filters
    float min_reputation;           // Minimum peer reputation
    int min_seeders;                // Minimum number of seeders
    bool verified_only;             // Only verified packages
    
    // Geographic preferences
    char* preferred_region;         // Preferred geographic region
    float max_distance_km;          // Maximum distance from user
    
    // Performance preferences
    int max_results;                // Maximum results to return
    int timeout_seconds;            // Search timeout
    bool parallel_search;           // Search multiple swarms in parallel
} P2PSearchQuery;

// Search result
typedef struct P2PSearchResult {
    P2PPackageAnnouncement* announcement;
    float relevance_score;          // Search relevance (0.0-1.0)
    char* match_reason;             // Why this matched
    
    // Download recommendations
    P2PPeer** recommended_peers;    // Best peers to download from
    size_t recommended_peer_count;
    
    struct P2PSearchResult* next;
} P2PSearchResult;

// P2P Discovery system
typedef struct P2PDiscovery {
    IpfsClient* ipfs_client;
    CryptoVerifier* crypto_verifier;
    
    // Peer management
    P2PPeer* known_peers;           // All known peers
    size_t peer_count;
    P2PPeer* trusted_peers;         // Trusted peers list
    P2PPeer* blocked_peers;         // Blocked peers list
    
    // Swarm management
    P2PSwarm* swarms;               // Active swarms
    size_t swarm_count;
    
    // Package announcements
    P2PPackageAnnouncement* announcements;
    size_t announcement_count;
    
    // Discovery configuration
    bool auto_discovery_enabled;    // Auto-discover peers
    bool bandwidth_sharing_enabled; // Share bandwidth
    bool cache_sharing_enabled;     // Share local cache
    float sharing_ratio_target;     // Target upload/download ratio
    
    // Security settings
    float min_peer_reputation;      // Minimum reputation to interact
    bool verify_all_packages;       // Verify all packages
    bool relay_enabled;             // Act as relay for others
    
    // Performance settings
    int max_concurrent_transfers;   // Max parallel transfers
    int announcement_ttl;           // Announcement time-to-live
    int peer_timeout;               // Peer timeout (seconds)
    size_t max_cache_size;          // Maximum cache size
    
    // Statistics
    size_t total_packages_shared;
    size_t total_packages_received;
    size_t total_bytes_uploaded;
    size_t total_bytes_downloaded;
} P2PDiscovery;

// Function declarations

// P2P Discovery lifecycle
P2PDiscovery* p2p_discovery_create(IpfsClient* ipfs_client, CryptoVerifier* verifier);
void p2p_discovery_free(P2PDiscovery* discovery);
bool p2p_discovery_initialize(P2PDiscovery* discovery);
bool p2p_discovery_start(P2PDiscovery* discovery);
bool p2p_discovery_stop(P2PDiscovery* discovery);

// Peer management
bool p2p_discovery_add_peer(P2PDiscovery* discovery, const char* peer_id, 
                           const char* multiaddr);
bool p2p_discovery_remove_peer(P2PDiscovery* discovery, const char* peer_id);
P2PPeer* p2p_discovery_get_peer(P2PDiscovery* discovery, const char* peer_id);
P2PPeer** p2p_discovery_list_peers(P2PDiscovery* discovery, size_t* peer_count);
bool p2p_discovery_update_peer_reputation(P2PDiscovery* discovery, const char* peer_id, 
                                         float delta);

// Swarm management
P2PSwarm* p2p_discovery_create_swarm(P2PDiscovery* discovery, const char* topic);
bool p2p_discovery_join_swarm(P2PDiscovery* discovery, const char* topic);
bool p2p_discovery_leave_swarm(P2PDiscovery* discovery, const char* topic);
P2PSwarm** p2p_discovery_list_swarms(P2PDiscovery* discovery, size_t* swarm_count);

// Package announcement and discovery
bool p2p_discovery_announce_package(P2PDiscovery* discovery, const char* package_name,
                                   const char* version, const IpfsCid* cid);
bool p2p_discovery_withdraw_announcement(P2PDiscovery* discovery, const char* package_name,
                                        const char* version);
P2PSearchResult** p2p_discovery_search_packages(P2PDiscovery* discovery, 
                                               P2PSearchQuery* query,
                                               size_t* result_count);

// Package sharing
bool p2p_discovery_request_package(P2PDiscovery* discovery, const char* peer_id,
                                  const IpfsCid* cid);
bool p2p_discovery_offer_package(P2PDiscovery* discovery, const IpfsCid* cid);
bool p2p_discovery_start_seeding(P2PDiscovery* discovery, const IpfsCid* cid);
bool p2p_discovery_stop_seeding(P2PDiscovery* discovery, const IpfsCid* cid);

// Bandwidth and cache sharing
bool p2p_discovery_donate_bandwidth(P2PDiscovery* discovery, float bandwidth_mbps);
bool p2p_discovery_share_cache(P2PDiscovery* discovery, const char* cache_key);
bool p2p_discovery_request_from_cache(P2PDiscovery* discovery, const char* peer_id,
                                     const char* cache_key);

// Trust and reputation
bool p2p_discovery_trust_peer(P2PDiscovery* discovery, const char* peer_id);
bool p2p_discovery_untrust_peer(P2PDiscovery* discovery, const char* peer_id);
bool p2p_discovery_block_peer(P2PDiscovery* discovery, const char* peer_id, 
                             const char* reason);
bool p2p_discovery_report_malicious(P2PDiscovery* discovery, const char* peer_id,
                                   const char* reason);

// Peer operations
P2PPeer* p2p_peer_create(const char* peer_id, const char* multiaddr);
void p2p_peer_free(P2PPeer* peer);
bool p2p_peer_update_stats(P2PPeer* peer, bool success, size_t bytes_transferred);
float p2p_peer_calculate_reputation(const P2PPeer* peer);
bool p2p_peer_is_reliable(const P2PPeer* peer);

// Announcement operations
P2PPackageAnnouncement* p2p_announcement_create(const char* package_name, 
                                               const char* version,
                                               const IpfsCid* cid);
void p2p_announcement_free(P2PPackageAnnouncement* announcement);
bool p2p_announcement_add_seeder(P2PPackageAnnouncement* announcement, P2PPeer* peer);
bool p2p_announcement_remove_seeder(P2PPackageAnnouncement* announcement, 
                                   const char* peer_id);
float p2p_announcement_calculate_availability(P2PPackageAnnouncement* announcement);

// Search operations
P2PSearchQuery* p2p_search_query_create(const char* query_string);
void p2p_search_query_free(P2PSearchQuery* query);
bool p2p_search_query_add_tag(P2PSearchQuery* query, const char* tag);
bool p2p_search_query_set_region(P2PSearchQuery* query, const char* region);

P2PSearchResult* p2p_search_result_create(P2PPackageAnnouncement* announcement);
void p2p_search_result_free(P2PSearchResult* result);

// NAT traversal and connectivity
typedef struct P2PRelay {
    P2PPeer* relay_peer;            // Peer acting as relay
    char* relay_address;            // Relay circuit address
    bool is_active;                 // Is relay active
    time_t established_at;          // When relay was established
} P2PRelay;

P2PRelay* p2p_discovery_request_relay(P2PDiscovery* discovery, const char* target_peer_id);
bool p2p_discovery_establish_direct_connection(P2PDiscovery* discovery, 
                                              const char* peer_id);
bool p2p_discovery_punch_nat(P2PDiscovery* discovery, const char* peer_id);

// DHT integration for peer discovery
bool p2p_discovery_announce_to_dht(P2PDiscovery* discovery, const char* key, 
                                  const char* value);
char** p2p_discovery_query_dht(P2PDiscovery* discovery, const char* key, 
                               size_t* result_count);
bool p2p_discovery_provide_to_dht(P2PDiscovery* discovery, const IpfsCid* cid);
P2PPeer** p2p_discovery_find_providers(P2PDiscovery* discovery, const IpfsCid* cid,
                                       size_t* provider_count);

// Gossip protocol for announcements
typedef struct P2PGossipMessage {
    char* message_id;               // Unique message ID
    char* topic;                    // Message topic
    char* content;                  // Message content (JSON)
    char* sender_peer_id;           // Original sender
    time_t timestamp;               // Message timestamp
    int hop_count;                  // Number of hops
} P2PGossipMessage;

bool p2p_discovery_gossip_subscribe(P2PDiscovery* discovery, const char* topic);
bool p2p_discovery_gossip_publish(P2PDiscovery* discovery, const char* topic,
                                 const char* message);
P2PGossipMessage** p2p_discovery_gossip_receive(P2PDiscovery* discovery, 
                                               const char* topic,
                                               size_t* message_count);

// Performance monitoring
typedef struct P2PStats {
    // Transfer statistics
    size_t packages_uploaded;
    size_t packages_downloaded;
    size_t bytes_uploaded;
    size_t bytes_downloaded;
    float avg_upload_speed;         // Average upload speed (MB/s)
    float avg_download_speed;       // Average download speed (MB/s)
    
    // Peer statistics
    size_t total_peers_discovered;
    size_t active_peers;
    size_t trusted_peers;
    size_t blocked_peers;
    
    // Swarm statistics
    size_t active_swarms;
    size_t total_announcements;
    size_t successful_discoveries;
    
    // Network health
    float network_reliability;      // Overall network reliability
    float average_peer_reputation;  // Average peer reputation
    time_t stats_period_start;
} P2PStats;

P2PStats* p2p_discovery_get_stats(P2PDiscovery* discovery);
void p2p_stats_free(P2PStats* stats);
bool p2p_discovery_reset_stats(P2PDiscovery* discovery);

// Configuration
typedef struct P2PConfig {
    // Discovery settings
    bool auto_discovery_enabled;
    char** bootstrap_peers;         // Initial peers to connect to
    size_t bootstrap_peer_count;
    
    // Sharing settings
    bool bandwidth_sharing_enabled;
    float bandwidth_limit_mbps;     // Upload bandwidth limit
    float sharing_ratio_target;
    bool cache_sharing_enabled;
    size_t cache_size_limit;
    
    // Security settings
    float min_peer_reputation;
    bool verify_all_packages;
    bool relay_enabled;
    int max_relay_connections;
    
    // Performance settings
    int max_concurrent_transfers;
    int announcement_ttl_minutes;
    int peer_timeout_seconds;
    int dht_refresh_interval;
} P2PConfig;

P2PConfig* p2p_config_create_default(void);
P2PConfig* p2p_config_load(const char* config_file);
bool p2p_config_save(const P2PConfig* config, const char* config_file);
void p2p_config_free(P2PConfig* config);

#endif // P2P_DISCOVERY_H