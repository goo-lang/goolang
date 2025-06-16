#ifndef HYBRID_REGISTRY_H
#define HYBRID_REGISTRY_H

#include "ipfs_client.h"
#include "ipfs_package.h"
#include "ipns_manager.h"
#include "gateway_intelligence.h"
#include "p2p_discovery.h"
#include "crypto_verifier.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Forward declarations
typedef struct HybridRegistry HybridRegistry;
typedef struct RegistryBackend RegistryBackend;
typedef struct PackageMetadata PackageMetadata;
typedef struct ResolutionStrategy ResolutionStrategy;
typedef struct DownloadPlan DownloadPlan;

// Registry backend types
typedef enum {
    BACKEND_TRADITIONAL,    // Traditional HTTP registry
    BACKEND_IPFS,          // IPFS content-addressed
    BACKEND_IPNS,          // IPNS mutable references
    BACKEND_P2P,           // P2P discovery network
    BACKEND_LOCAL_CACHE,   // Local filesystem cache
    BACKEND_MIRROR,        // Mirror/CDN backend
    BACKEND_BLOCKCHAIN     // Blockchain-based registry
} BackendType;

// Registry backend configuration
typedef struct RegistryBackend {
    BackendType type;
    char* name;                     // Backend name
    char* url;                      // Backend URL/endpoint
    
    // Backend capabilities
    bool supports_search;           // Can search packages
    bool supports_metadata;         // Stores package metadata
    bool supports_binary;           // Stores binary content
    bool supports_versioning;       // Version management
    bool supports_signing;          // Cryptographic signatures
    
    // Performance characteristics
    float avg_latency_ms;           // Average latency
    float bandwidth_mbps;           // Available bandwidth
    float reliability_score;        // Reliability (0.0-1.0)
    int priority;                   // Priority order
    
    // Authentication
    char* api_key;                  // API key if required
    char* auth_token;               // Authentication token
    
    // Statistics
    size_t requests_sent;
    size_t requests_succeeded;
    size_t bytes_downloaded;
    time_t last_used;
    
    struct RegistryBackend* next;   // Linked list
} RegistryBackend;

// Enhanced package metadata
typedef struct PackageMetadata {
    // Basic information
    char* name;                     // Package name
    char* version;                  // Package version
    char* description;              // Package description
    char* license;                  // License identifier
    char** authors;                 // Package authors
    size_t author_count;
    
    // Content identifiers
    IpfsCid* ipfs_cid;             // IPFS content ID
    char* ipns_name;               // IPNS mutable name
    char* traditional_url;          // Traditional download URL
    char* checksum_sha256;          // SHA256 checksum
    
    // Dependencies
    char** dependencies;            // Dependency list
    size_t dependency_count;
    char** dev_dependencies;        // Dev dependencies
    size_t dev_dependency_count;
    
    // Binary artifacts
    typedef struct {
        char* platform;             // Target platform
        char* architecture;         // Target architecture
        IpfsCid* binary_cid;       // Binary content ID
        char* download_url;         // Alternative URL
        size_t size;               // Binary size
    } BinaryArtifact;
    
    BinaryArtifact** binaries;     // Pre-built binaries
    size_t binary_count;
    
    // Metadata timestamps
    time_t published_at;            // Publication time
    time_t updated_at;              // Last update time
    
    // Security information
    PackageSignature** signatures;  // Package signatures
    size_t signature_count;
    bool is_verified;               // Verification status
    char* vulnerability_report;     // Known vulnerabilities
    
    // Registry information
    RegistryBackend* primary_registry; // Primary registry
    RegistryBackend** mirrors;      // Mirror locations
    size_t mirror_count;
} PackageMetadata;

// Resolution strategy configuration
typedef struct ResolutionStrategy {
    // Strategy type
    enum {
        STRATEGY_FASTEST_FIRST,     // Try fastest backend first
        STRATEGY_MOST_RELIABLE,     // Try most reliable first
        STRATEGY_PARALLEL,          // Try all in parallel
        STRATEGY_FAILOVER,          // Sequential failover
        STRATEGY_COST_OPTIMIZED,    // Minimize bandwidth cost
        STRATEGY_HYBRID_SMART       // AI-driven selection
    } type;
    
    // Strategy parameters
    int max_parallel_requests;      // For parallel strategy
    int timeout_per_backend;        // Timeout per backend (seconds)
    bool prefer_ipfs;              // Prefer IPFS when available
    bool prefer_p2p;               // Prefer P2P sources
    bool require_signatures;        // Require signed packages
    
    // Fallback configuration
    bool enable_fallback;           // Enable fallback to other backends
    int max_fallback_attempts;      // Maximum fallback attempts
    BackendType* fallback_order;    // Fallback priority order
    size_t fallback_count;
} ResolutionStrategy;

// Download plan for optimized retrieval
typedef struct DownloadPlan {
    PackageMetadata* package;       // Package to download
    
    // Download sources (ordered by preference)
    typedef struct {
        BackendType backend_type;
        char* source_url;           // URL or CID
        float expected_speed;       // Expected speed (MB/s)
        float reliability;          // Source reliability
        bool is_verified;           // Pre-verified source
    } DownloadSource;
    
    DownloadSource** sources;       // Available sources
    size_t source_count;
    
    // Download strategy
    bool use_parallel_chunks;       // Download in parallel chunks
    int chunk_size;                 // Chunk size for parallel download
    bool verify_while_downloading;  // Stream verification
    
    // Estimated metrics
    float estimated_time_seconds;   // Estimated download time
    size_t total_size;             // Total download size
    
    // Execution state
    bool is_executing;              // Currently downloading
    float progress_percentage;      // Download progress
    size_t bytes_downloaded;        // Bytes downloaded so far
} DownloadPlan;

// Hybrid Registry system
typedef struct HybridRegistry {
    // Backend components
    IpfsClient* ipfs_client;
    IpnsManager* ipns_manager;
    GatewayIntelligence* gateway_intelligence;
    P2PDiscovery* p2p_discovery;
    CryptoVerifier* crypto_verifier;
    
    // Registry backends
    RegistryBackend* backends;      // All configured backends
    size_t backend_count;
    
    // Package cache
    PackageMetadata** cached_metadata;
    size_t cache_size;
    size_t cache_capacity;
    char* cache_directory;          // Local cache directory
    
    // Resolution configuration
    ResolutionStrategy* default_strategy;
    bool auto_discover_registries;  // Auto-discover new registries
    bool enable_blockchain_verification; // Use blockchain for verification
    
    // Performance optimization
    bool enable_predictive_caching; // Predictive package caching
    bool enable_bandwidth_sharing;  // Share bandwidth with P2P
    int max_concurrent_downloads;   // Maximum parallel downloads
    
    // Security configuration
    bool require_https;             // Require HTTPS for traditional registries
    bool verify_all_downloads;      // Verify all downloaded packages
    float min_trust_score;          // Minimum trust score for sources
    
    // Statistics
    size_t total_packages_resolved;
    size_t total_packages_downloaded;
    size_t cache_hits;
    size_t cache_misses;
    float avg_resolution_time;
    float avg_download_speed;
} HybridRegistry;

// Function declarations

// Hybrid Registry lifecycle
HybridRegistry* hybrid_registry_create(void);
void hybrid_registry_free(HybridRegistry* registry);
bool hybrid_registry_initialize(HybridRegistry* registry);

// Backend management
bool hybrid_registry_add_backend(HybridRegistry* registry, RegistryBackend* backend);
bool hybrid_registry_remove_backend(HybridRegistry* registry, const char* backend_name);
RegistryBackend* hybrid_registry_get_backend(HybridRegistry* registry, const char* name);
RegistryBackend** hybrid_registry_list_backends(HybridRegistry* registry, size_t* count);
bool hybrid_registry_test_backend(HybridRegistry* registry, RegistryBackend* backend);

// Package resolution
PackageMetadata* hybrid_registry_resolve_package(HybridRegistry* registry, 
                                               const char* package_name,
                                               const char* version_spec);
PackageMetadata** hybrid_registry_search_packages(HybridRegistry* registry,
                                                const char* query,
                                                size_t* result_count);
bool hybrid_registry_refresh_metadata(HybridRegistry* registry, 
                                    PackageMetadata* metadata);

// Download operations
DownloadPlan* hybrid_registry_create_download_plan(HybridRegistry* registry,
                                                  PackageMetadata* package);
bool hybrid_registry_execute_download(HybridRegistry* registry, 
                                    DownloadPlan* plan,
                                    const char* destination);
bool hybrid_registry_verify_download(HybridRegistry* registry,
                                   const char* file_path,
                                   PackageMetadata* metadata);

// Publishing operations
bool hybrid_registry_publish_package(HybridRegistry* registry,
                                   const char* package_path,
                                   PackageMetadata* metadata);
bool hybrid_registry_publish_to_ipfs(HybridRegistry* registry,
                                   const char* package_path,
                                   PackageMetadata* metadata);
bool hybrid_registry_update_ipns(HybridRegistry* registry,
                               const char* ipns_name,
                               const IpfsCid* new_cid);

// Cache management
bool hybrid_registry_cache_package(HybridRegistry* registry,
                                 PackageMetadata* metadata,
                                 const char* file_path);
char* hybrid_registry_get_cached_path(HybridRegistry* registry,
                                    const char* package_name,
                                    const char* version);
bool hybrid_registry_clean_cache(HybridRegistry* registry,
                               size_t max_size_bytes);

// Strategy management
ResolutionStrategy* hybrid_registry_create_strategy(void);
void hybrid_registry_free_strategy(ResolutionStrategy* strategy);
bool hybrid_registry_set_strategy(HybridRegistry* registry, 
                                ResolutionStrategy* strategy);

// Backend operations
RegistryBackend* registry_backend_create(BackendType type, const char* name, 
                                       const char* url);
void registry_backend_free(RegistryBackend* backend);
bool registry_backend_authenticate(RegistryBackend* backend, 
                                 const char* api_key,
                                 const char* auth_token);
bool registry_backend_update_stats(RegistryBackend* backend,
                                 bool success,
                                 float latency_ms,
                                 size_t bytes_transferred);

// Metadata operations
PackageMetadata* package_metadata_create(const char* name, const char* version);
void package_metadata_free(PackageMetadata* metadata);
bool package_metadata_add_dependency(PackageMetadata* metadata, 
                                   const char* dependency);
bool package_metadata_add_binary(PackageMetadata* metadata,
                               const char* platform,
                               const char* architecture,
                               const IpfsCid* binary_cid);
char* package_metadata_serialize(const PackageMetadata* metadata);
PackageMetadata* package_metadata_deserialize(const char* json_data);

// Advanced features

// Blockchain integration for immutable registry
typedef struct BlockchainRegistry {
    char* contract_address;         // Smart contract address
    char* blockchain_network;       // Ethereum, Polygon, etc.
    char* rpc_endpoint;            // Blockchain RPC endpoint
} BlockchainRegistry;

bool hybrid_registry_enable_blockchain(HybridRegistry* registry,
                                     BlockchainRegistry* blockchain);
bool hybrid_registry_verify_on_blockchain(HybridRegistry* registry,
                                        PackageMetadata* metadata);
bool hybrid_registry_publish_to_blockchain(HybridRegistry* registry,
                                         PackageMetadata* metadata);

// AI-driven optimization
typedef struct AIOptimizer {
    // Machine learning model for source selection
    void* ml_model;                 // Trained ML model
    float* feature_weights;         // Feature importance weights
    
    // Historical data
    float** download_history;       // Download performance history
    size_t history_size;
    
    // Predictions
    float (*predict_download_time)(struct AIOptimizer* optimizer,
                                 RegistryBackend* backend,
                                 size_t package_size);
    RegistryBackend* (*select_optimal_backend)(struct AIOptimizer* optimizer,
                                              PackageMetadata* package,
                                              HybridRegistry* registry);
} AIOptimizer;

AIOptimizer* ai_optimizer_create(void);
void ai_optimizer_free(AIOptimizer* optimizer);
bool ai_optimizer_train(AIOptimizer* optimizer, HybridRegistry* registry);
bool hybrid_registry_enable_ai_optimization(HybridRegistry* registry,
                                          AIOptimizer* optimizer);

// Multi-registry federation
typedef struct RegistryFederation {
    HybridRegistry** member_registries;
    size_t member_count;
    
    // Federation protocol
    bool (*negotiate_package_location)(struct RegistryFederation* federation,
                                     const char* package_name,
                                     const char* version);
    bool (*coordinate_replication)(struct RegistryFederation* federation,
                                 PackageMetadata* metadata);
} RegistryFederation;

RegistryFederation* registry_federation_create(void);
bool registry_federation_add_member(RegistryFederation* federation,
                                  HybridRegistry* registry);
bool registry_federation_replicate_package(RegistryFederation* federation,
                                         PackageMetadata* metadata,
                                         int replication_factor);

// Performance monitoring
typedef struct RegistryStats {
    // Backend performance
    typedef struct {
        char* backend_name;
        float avg_latency_ms;
        float success_rate;
        size_t total_requests;
        size_t bytes_transferred;
    } BackendStats;
    
    BackendStats** backend_stats;
    size_t backend_count;
    
    // Overall statistics
    float avg_resolution_time_ms;
    float avg_download_speed_mbps;
    float cache_hit_rate;
    size_t total_downloads;
    
    // Time-based metrics
    time_t stats_period_start;
    time_t stats_period_end;
} RegistryStats;

RegistryStats* hybrid_registry_get_stats(HybridRegistry* registry);
void registry_stats_free(RegistryStats* stats);
bool hybrid_registry_export_stats(HybridRegistry* registry, const char* output_file);

// Configuration management
typedef struct HybridRegistryConfig {
    // Backend configuration
    RegistryBackend** backends;
    size_t backend_count;
    
    // Strategy configuration
    ResolutionStrategy* default_strategy;
    
    // Cache configuration
    char* cache_directory;
    size_t max_cache_size;
    
    // Security configuration
    bool require_https;
    bool verify_all_downloads;
    float min_trust_score;
    
    // Performance configuration
    bool enable_predictive_caching;
    bool enable_bandwidth_sharing;
    int max_concurrent_downloads;
} HybridRegistryConfig;

HybridRegistryConfig* hybrid_registry_config_create_default(void);
HybridRegistryConfig* hybrid_registry_config_load(const char* config_file);
bool hybrid_registry_config_save(const HybridRegistryConfig* config, 
                               const char* config_file);
void hybrid_registry_config_free(HybridRegistryConfig* config);

#endif // HYBRID_REGISTRY_H