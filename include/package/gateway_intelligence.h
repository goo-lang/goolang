#ifndef GATEWAY_INTELLIGENCE_H
#define GATEWAY_INTELLIGENCE_H

#include "ipfs_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Forward declarations
typedef struct GatewayIntelligence GatewayIntelligence;
typedef struct GatewayMetrics GatewayMetrics;
typedef struct GatewayRanking GatewayRanking;
typedef struct NetworkConditions NetworkConditions;

// Gateway performance metrics
typedef struct GatewayMetrics {
    char* gateway_url;
    
    // Performance metrics
    float avg_response_time;     // Average response time (ms)
    float min_response_time;     // Fastest response (ms)
    float max_response_time;     // Slowest response (ms)
    float std_dev_response_time; // Response time variance
    
    // Reliability metrics
    float uptime_percentage;     // Uptime over last 24h
    int successful_requests;     // Successful requests count
    int failed_requests;         // Failed requests count
    int timeout_requests;        // Timed out requests
    
    // Bandwidth metrics
    float avg_download_speed;    // Average download speed (MB/s)
    float peak_download_speed;   // Peak download speed (MB/s)
    size_t total_bytes_downloaded; // Total bytes downloaded
    
    // Geographic and network metrics
    char* geographic_region;     // Estimated geographic region
    float network_latency;       // Network latency (ms)
    float packet_loss_rate;      // Packet loss percentage
    
    // Content availability
    float content_hit_rate;      // Percentage of content found
    int cache_freshness_score;   // How up-to-date content is
    
    // Time-based metrics
    time_t last_updated;         // Last metrics update
    time_t first_seen;          // When gateway was first discovered
    int measurement_count;       // Number of measurements taken
    
    // Quality scores (0.0-1.0)
    float performance_score;     // Overall performance
    float reliability_score;     // Overall reliability
    float quality_score;         // Combined quality score
} GatewayMetrics;

// Network condition detection
typedef struct NetworkConditions {
    float bandwidth_estimate;    // Estimated bandwidth (Mbps)
    float latency_estimate;      // Estimated latency (ms)
    bool is_mobile_network;      // Mobile vs fixed connection
    bool is_metered_connection;  // Metered connection
    char* country_code;          // User's country code
    char* isp_name;             // Internet service provider
    bool behind_corporate_firewall; // Corporate network detection
    float network_quality_score; // Overall network quality (0.0-1.0)
} NetworkConditions;

// Gateway ranking with multiple criteria
typedef struct GatewayRanking {
    IpfsGateway* gateway;
    GatewayMetrics* metrics;
    
    // Ranking scores
    float performance_rank;      // Performance ranking (0.0-1.0)
    float reliability_rank;      // Reliability ranking (0.0-1.0)
    float geographic_rank;       // Geographic proximity ranking
    float cost_efficiency_rank;  // Cost efficiency (for paid gateways)
    
    // Context-aware rankings
    float mobile_optimized_rank; // Mobile network optimization
    float corporate_friendly_rank; // Corporate firewall friendliness
    float privacy_rank;          // Privacy protection level
    
    // Final composite score
    float composite_score;       // Weighted final score
    char* recommendation_reason; // Why this gateway was recommended
} GatewayRanking;

// Gateway intelligence system
typedef struct GatewayIntelligence {
    IpfsClient* ipfs_client;
    
    // Gateway discovery and management
    IpfsGateway** discovered_gateways; // All discovered gateways
    size_t gateway_count;
    size_t gateway_capacity;
    
    // Metrics collection
    GatewayMetrics** metrics;    // Metrics for each gateway
    size_t metrics_count;
    
    // Network condition monitoring
    NetworkConditions* network_conditions;
    
    // Intelligence configuration
    bool enable_auto_discovery;  // Auto-discover new gateways
    bool enable_geo_optimization; // Optimize for geographic proximity
    bool enable_cost_optimization; // Consider cost in ranking
    bool enable_privacy_mode;    // Prioritize privacy-focused gateways
    
    // Learning parameters
    float learning_rate;         // How quickly to adapt to changes
    int measurement_window;      // Time window for metrics (hours)
    int min_measurements;        // Minimum measurements before ranking
    
    // Ranking weights
    float performance_weight;    // Weight for performance (speed)
    float reliability_weight;    // Weight for reliability (uptime)
    float geographic_weight;     // Weight for geographic proximity
    float cost_weight;          // Weight for cost efficiency
    float privacy_weight;       // Weight for privacy protection
} GatewayIntelligence;

// Function declarations

// Gateway Intelligence lifecycle
GatewayIntelligence* gateway_intelligence_create(IpfsClient* ipfs_client);
void gateway_intelligence_free(GatewayIntelligence* intelligence);
bool gateway_intelligence_initialize(GatewayIntelligence* intelligence);

// Gateway discovery
bool gateway_intelligence_discover_gateways(GatewayIntelligence* intelligence);
bool gateway_intelligence_add_gateway(GatewayIntelligence* intelligence, const char* gateway_url);
bool gateway_intelligence_remove_gateway(GatewayIntelligence* intelligence, const char* gateway_url);
IpfsGateway** gateway_intelligence_get_all_gateways(GatewayIntelligence* intelligence, size_t* count);

// Metrics collection and monitoring
bool gateway_intelligence_collect_metrics(GatewayIntelligence* intelligence);
GatewayMetrics* gateway_intelligence_measure_gateway(GatewayIntelligence* intelligence, 
                                                   IpfsGateway* gateway);
bool gateway_intelligence_update_metrics(GatewayIntelligence* intelligence, 
                                        const char* gateway_url, 
                                        GatewayMetrics* new_metrics);

// Network condition detection
NetworkConditions* gateway_intelligence_detect_network_conditions(GatewayIntelligence* intelligence);
bool gateway_intelligence_update_network_conditions(GatewayIntelligence* intelligence);

// Gateway ranking and selection
GatewayRanking** gateway_intelligence_rank_gateways(GatewayIntelligence* intelligence, 
                                                   size_t* ranking_count);
IpfsGateway* gateway_intelligence_select_best_gateway(GatewayIntelligence* intelligence);
IpfsGateway* gateway_intelligence_select_for_content(GatewayIntelligence* intelligence, 
                                                   const IpfsCid* cid);
IpfsGateway* gateway_intelligence_select_for_region(GatewayIntelligence* intelligence, 
                                                  const char* region);

// Adaptive learning
bool gateway_intelligence_learn_from_experience(GatewayIntelligence* intelligence, 
                                               const char* gateway_url,
                                               bool success, 
                                               float response_time, 
                                               size_t bytes_transferred);
bool gateway_intelligence_adapt_to_network_changes(GatewayIntelligence* intelligence);

// Gateway metrics operations
GatewayMetrics* gateway_metrics_create(const char* gateway_url);
void gateway_metrics_free(GatewayMetrics* metrics);
bool gateway_metrics_update(GatewayMetrics* metrics, 
                           float response_time, 
                           bool success, 
                           size_t bytes_transferred);
float gateway_metrics_calculate_quality_score(const GatewayMetrics* metrics);

// Network condition operations
NetworkConditions* network_conditions_create(void);
void network_conditions_free(NetworkConditions* conditions);
bool network_conditions_detect(NetworkConditions* conditions);
bool network_conditions_is_mobile_friendly(const NetworkConditions* conditions);
bool network_conditions_is_corporate_friendly(const NetworkConditions* conditions);

// Gateway ranking operations
GatewayRanking* gateway_ranking_create(IpfsGateway* gateway, GatewayMetrics* metrics);
void gateway_ranking_free(GatewayRanking* ranking);
float gateway_ranking_calculate_composite_score(GatewayRanking* ranking, 
                                               const NetworkConditions* network_conditions,
                                               const GatewayIntelligence* intelligence);

// Specialized selection algorithms
IpfsGateway* gateway_select_fastest(GatewayIntelligence* intelligence);
IpfsGateway* gateway_select_most_reliable(GatewayIntelligence* intelligence);
IpfsGateway* gateway_select_nearest(GatewayIntelligence* intelligence);
IpfsGateway* gateway_select_privacy_focused(GatewayIntelligence* intelligence);
IpfsGateway* gateway_select_cost_effective(GatewayIntelligence* intelligence);

// Load balancing and failover
typedef struct GatewayLoadBalancer {
    GatewayIntelligence* intelligence;
    IpfsGateway** active_gateways;   // Currently active gateways
    size_t active_count;
    float* load_distribution;        // Load distribution per gateway
    time_t last_rebalance;          // Last load rebalancing
} GatewayLoadBalancer;

GatewayLoadBalancer* gateway_load_balancer_create(GatewayIntelligence* intelligence);
void gateway_load_balancer_free(GatewayLoadBalancer* balancer);
IpfsGateway* gateway_load_balancer_select_next(GatewayLoadBalancer* balancer);
bool gateway_load_balancer_rebalance(GatewayLoadBalancer* balancer);
bool gateway_load_balancer_handle_failure(GatewayLoadBalancer* balancer, 
                                         IpfsGateway* failed_gateway);

// Performance optimization
typedef struct GatewayOptimizer {
    GatewayIntelligence* intelligence;
    
    // Optimization strategies
    bool enable_parallel_testing;   // Test multiple gateways in parallel
    bool enable_content_routing;    // Route based on content availability
    bool enable_bandwidth_adaptation; // Adapt to bandwidth changes
    bool enable_failure_prediction; // Predict gateway failures
    
    // Machine learning components
    float* gateway_weights;         // Learned weights for each gateway
    float* feature_importance;      // Feature importance scores
    int training_samples;          // Number of training samples
} GatewayOptimizer;

GatewayOptimizer* gateway_optimizer_create(GatewayIntelligence* intelligence);
void gateway_optimizer_free(GatewayOptimizer* optimizer);
bool gateway_optimizer_train(GatewayOptimizer* optimizer);
IpfsGateway* gateway_optimizer_predict_best(GatewayOptimizer* optimizer, 
                                          const IpfsCid* cid,
                                          const NetworkConditions* conditions);

// Configuration and persistence
typedef struct GatewayIntelligenceConfig {
    // Discovery settings
    bool auto_discovery_enabled;
    char** seed_gateways;           // Initial gateway list
    size_t seed_gateway_count;
    int discovery_interval;         // Discovery interval (minutes)
    
    // Measurement settings
    int measurement_interval;       // Measurement interval (minutes)
    int measurement_timeout;        // Measurement timeout (seconds)
    int max_concurrent_measurements; // Max parallel measurements
    
    // Ranking weights
    float performance_weight;
    float reliability_weight;
    float geographic_weight;
    float cost_weight;
    float privacy_weight;
    
    // Learning parameters
    float learning_rate;
    int measurement_window_hours;
    int min_measurements_for_ranking;
    
    // Optimization flags
    bool enable_geo_optimization;
    bool enable_cost_optimization;
    bool enable_privacy_mode;
    bool enable_mobile_optimization;
} GatewayIntelligenceConfig;

GatewayIntelligenceConfig* gateway_intelligence_config_create_default(void);
GatewayIntelligenceConfig* gateway_intelligence_config_load(const char* config_file);
bool gateway_intelligence_config_save(const GatewayIntelligenceConfig* config, 
                                     const char* config_file);
void gateway_intelligence_config_free(GatewayIntelligenceConfig* config);

// Statistics and reporting
typedef struct GatewayIntelligenceStats {
    size_t total_gateways_discovered;
    size_t active_gateways;
    size_t measurements_taken;
    float avg_gateway_quality;
    char* best_gateway_url;
    char* geographic_distribution[10]; // Top 10 geographic regions
    time_t stats_generated_at;
} GatewayIntelligenceStats;

GatewayIntelligenceStats* gateway_intelligence_get_stats(GatewayIntelligence* intelligence);
void gateway_intelligence_stats_free(GatewayIntelligenceStats* stats);
bool gateway_intelligence_export_metrics(GatewayIntelligence* intelligence, 
                                        const char* output_file);

#endif // GATEWAY_INTELLIGENCE_H