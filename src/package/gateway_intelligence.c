#include "package/gateway_intelligence.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sys/time.h>

// Response data structure for HTTP requests
typedef struct {
    char* data;
    size_t size;
} HTTPResponse;

// Write callback for curl
static size_t write_callback(void* contents, size_t size, size_t nmemb, HTTPResponse* response) {
    size_t real_size = size * nmemb;
    char* ptr = realloc(response->data, response->size + real_size + 1);
    if (!ptr) return 0;
    
    response->data = ptr;
    memcpy(&(response->data[response->size]), contents, real_size);
    response->size += real_size;
    response->data[response->size] = '\0';
    
    return real_size;
}

// Get current time in milliseconds
static long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)(tv.tv_sec) * 1000 + (long long)(tv.tv_usec) / 1000;
}

GatewayIntelligence* gateway_intelligence_create(IpfsClient* ipfs_client) {
    GatewayIntelligence* intelligence = xcalloc(1, sizeof(GatewayIntelligence));
    if (!intelligence) return NULL;
    
    intelligence->ipfs_client = ipfs_client;
    intelligence->gateway_capacity = 32;
    intelligence->discovered_gateways = calloc(intelligence->gateway_capacity, sizeof(IpfsGateway*));
    intelligence->metrics = calloc(intelligence->gateway_capacity, sizeof(GatewayMetrics*));
    intelligence->network_conditions = network_conditions_create();
    
    // Default configuration
    intelligence->enable_auto_discovery = true;
    intelligence->enable_geo_optimization = true;
    intelligence->enable_cost_optimization = false;
    intelligence->enable_privacy_mode = false;
    
    // Default learning parameters
    intelligence->learning_rate = 0.1f;
    intelligence->measurement_window = 24;
    intelligence->min_measurements = 3;
    
    // Default ranking weights
    intelligence->performance_weight = 0.4f;
    intelligence->reliability_weight = 0.3f;
    intelligence->geographic_weight = 0.2f;
    intelligence->cost_weight = 0.05f;
    intelligence->privacy_weight = 0.05f;
    
    return intelligence;
}

void gateway_intelligence_free(GatewayIntelligence* intelligence) {
    if (!intelligence) return;
    
    // Free discovered gateways
    for (size_t i = 0; i < intelligence->gateway_count; i++) {
        if (intelligence->discovered_gateways[i]) {
            ipfs_gateway_free(intelligence->discovered_gateways[i]);
        }
    }
    free(intelligence->discovered_gateways);
    
    // Free metrics
    for (size_t i = 0; i < intelligence->metrics_count; i++) {
        gateway_metrics_free(intelligence->metrics[i]);
    }
    free(intelligence->metrics);
    
    network_conditions_free(intelligence->network_conditions);
    free(intelligence);
}

bool gateway_intelligence_initialize(GatewayIntelligence* intelligence) {
    if (!intelligence) return false;
    
    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Detect network conditions
    if (!gateway_intelligence_update_network_conditions(intelligence)) {
        return false;
    }
    
    // Discover initial gateways
    if (intelligence->enable_auto_discovery) {
        gateway_intelligence_discover_gateways(intelligence);
    }
    
    return true;
}

bool gateway_intelligence_discover_gateways(GatewayIntelligence* intelligence) {
    if (!intelligence) return false;
    
    // Well-known IPFS gateways
    const char* known_gateways[] = {
        "https://ipfs.io",
        "https://dweb.link",
        "https://cf-ipfs.com",
        "https://gateway.pinata.cloud",
        "https://ipfs.infura.io",
        "https://hardbin.com",
        "https://cloudflare-ipfs.com",
        "https://gateway.ipfs.io",
        "http://127.0.0.1:8080",  // Local node
        NULL
    };
    
    for (int i = 0; known_gateways[i]; i++) {
        gateway_intelligence_add_gateway(intelligence, known_gateways[i]);
    }
    
    // TODO: Implement dynamic discovery via DHT, DNS-over-HTTPS, etc.
    
    return true;
}

bool gateway_intelligence_add_gateway(GatewayIntelligence* intelligence, const char* gateway_url) {
    if (!intelligence || !gateway_url) return false;
    
    // Check if we need to expand capacity
    if (intelligence->gateway_count >= intelligence->gateway_capacity) {
        size_t new_capacity = intelligence->gateway_capacity * 2;
        IpfsGateway** new_gateways = realloc(intelligence->discovered_gateways, 
                                           new_capacity * sizeof(IpfsGateway*));
        GatewayMetrics** new_metrics = realloc(intelligence->metrics,
                                             new_capacity * sizeof(GatewayMetrics*));
        if (!new_gateways || !new_metrics) return false;
        
        intelligence->discovered_gateways = new_gateways;
        intelligence->metrics = new_metrics;
        intelligence->gateway_capacity = new_capacity;
    }
    
    // Create gateway and metrics
    IpfsGateway* gateway = ipfs_gateway_create(gateway_url);
    if (!gateway) return false;
    
    GatewayMetrics* metrics = gateway_metrics_create(gateway_url);
    if (!metrics) {
        ipfs_gateway_free(gateway);
        return false;
    }
    
    intelligence->discovered_gateways[intelligence->gateway_count] = gateway;
    intelligence->metrics[intelligence->gateway_count] = metrics;
    intelligence->gateway_count++;
    intelligence->metrics_count++;
    
    return true;
}

GatewayMetrics* gateway_intelligence_measure_gateway(GatewayIntelligence* intelligence, 
                                                   IpfsGateway* gateway) {
    if (!intelligence || !gateway) return NULL;
    
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    
    HTTPResponse response = {0};
    long long start_time = get_time_ms();
    
    // Test with a small known IPFS hash
    char test_url[512];
    snprintf(test_url, sizeof(test_url), "%s/ipfs/QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG", 
             gateway->url);
    
    curl_easy_setopt(curl, CURLOPT_URL, test_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Goo-Package-Manager/1.0");
    
    CURLcode res = curl_easy_perform(curl);
    long long end_time = get_time_ms();
    float response_time = (float)(end_time - start_time);
    
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    
    double download_speed = 0;
    curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD, &download_speed);
    
    curl_easy_cleanup(curl);
    
    // Find existing metrics or create new ones
    GatewayMetrics* metrics = NULL;
    for (size_t i = 0; i < intelligence->metrics_count; i++) {
        if (intelligence->metrics[i] && 
            strcmp(intelligence->metrics[i]->gateway_url, gateway->url) == 0) {
            metrics = intelligence->metrics[i];
            break;
        }
    }
    
    if (!metrics) {
        metrics = gateway_metrics_create(gateway->url);
        if (!metrics) {
            free(response.data);
            return NULL;
        }
    }
    
    // Update metrics
    bool success = (res == CURLE_OK && response_code == 200);
    gateway_metrics_update(metrics, response_time, success, response.size);
    
    free(response.data);
    return metrics;
}

bool gateway_metrics_update(GatewayMetrics* metrics, 
                           float response_time, 
                           bool success, 
                           size_t bytes_transferred) {
    if (!metrics) return false;
    
    metrics->measurement_count++;
    
    if (success) {
        metrics->successful_requests++;
        
        // Update response time statistics
        if (metrics->measurement_count == 1) {
            metrics->avg_response_time = response_time;
            metrics->min_response_time = response_time;
            metrics->max_response_time = response_time;
        } else {
            float alpha = 0.1f; // Exponential moving average factor
            metrics->avg_response_time = (1 - alpha) * metrics->avg_response_time + alpha * response_time;
            
            if (response_time < metrics->min_response_time) {
                metrics->min_response_time = response_time;
            }
            if (response_time > metrics->max_response_time) {
                metrics->max_response_time = response_time;
            }
        }
        
        // Update bandwidth metrics
        if (bytes_transferred > 0 && response_time > 0) {
            float speed_mbps = (bytes_transferred / 1024.0f / 1024.0f) / (response_time / 1000.0f);
            if (metrics->successful_requests == 1) {
                metrics->avg_download_speed = speed_mbps;
                metrics->peak_download_speed = speed_mbps;
            } else {
                float alpha = 0.1f;
                metrics->avg_download_speed = (1 - alpha) * metrics->avg_download_speed + alpha * speed_mbps;
                if (speed_mbps > metrics->peak_download_speed) {
                    metrics->peak_download_speed = speed_mbps;
                }
            }
        }
        
        metrics->total_bytes_downloaded += bytes_transferred;
    } else {
        metrics->failed_requests++;
    }
    
    // Calculate uptime percentage
    metrics->uptime_percentage = (float)metrics->successful_requests / 
                                (float)(metrics->successful_requests + metrics->failed_requests);
    
    // Update quality scores
    metrics->performance_score = gateway_metrics_calculate_quality_score(metrics);
    metrics->reliability_score = metrics->uptime_percentage;
    metrics->quality_score = (metrics->performance_score + metrics->reliability_score) / 2.0f;
    
    metrics->last_updated = time(NULL);
    
    return true;
}

float gateway_metrics_calculate_quality_score(const GatewayMetrics* metrics) {
    if (!metrics || metrics->measurement_count == 0) return 0.0f;
    
    // Performance score based on response time and download speed
    float response_score = 1.0f;
    if (metrics->avg_response_time > 0) {
        // Score decreases exponentially with response time
        response_score = expf(-metrics->avg_response_time / 1000.0f);
    }
    
    float speed_score = 1.0f;
    if (metrics->avg_download_speed > 0) {
        // Normalize speed to 0-1 range (assuming max desirable speed of 10 MB/s)
        speed_score = fminf(metrics->avg_download_speed / 10.0f, 1.0f);
    }
    
    return (response_score + speed_score) / 2.0f;
}

GatewayMetrics* gateway_metrics_create(const char* gateway_url) {
    if (!gateway_url) return NULL;
    
    GatewayMetrics* metrics = xcalloc(1, sizeof(GatewayMetrics));
    if (!metrics) return NULL;
    
    metrics->gateway_url = strdup(gateway_url);
    if (!metrics->gateway_url) {
        free(metrics);
        return NULL;
    }
    
    metrics->first_seen = time(NULL);
    return metrics;
}

void gateway_metrics_free(GatewayMetrics* metrics) {
    if (!metrics) return;
    
    free(metrics->gateway_url);
    free(metrics->geographic_region);
    free(metrics);
}

NetworkConditions* network_conditions_create(void) {
    NetworkConditions* conditions = xcalloc(1, sizeof(NetworkConditions));
    if (!conditions) return NULL;
    
    // Default values
    conditions->bandwidth_estimate = 10.0f; // 10 Mbps default
    conditions->latency_estimate = 50.0f;   // 50ms default
    conditions->network_quality_score = 0.5f;
    
    return conditions;
}

void network_conditions_free(NetworkConditions* conditions) {
    if (!conditions) return;
    
    free(conditions->country_code);
    free(conditions->isp_name);
    free(conditions);
}

bool network_conditions_detect(NetworkConditions* conditions) {
    if (!conditions) return false;
    
    // TODO: Implement network condition detection
    // This would involve:
    // 1. Bandwidth estimation via speed tests
    // 2. Latency measurement to various endpoints
    // 3. Mobile network detection via user-agent or network interfaces
    // 4. Geographic location detection via IP geolocation
    // 5. Corporate firewall detection via connectivity tests
    
    return true;
}

bool gateway_intelligence_update_network_conditions(GatewayIntelligence* intelligence) {
    if (!intelligence || !intelligence->network_conditions) return false;
    
    return network_conditions_detect(intelligence->network_conditions);
}

IpfsGateway* gateway_intelligence_select_best_gateway(GatewayIntelligence* intelligence) {
    if (!intelligence || intelligence->gateway_count == 0) return NULL;
    
    size_t ranking_count;
    GatewayRanking** rankings = gateway_intelligence_rank_gateways(intelligence, &ranking_count);
    if (!rankings || ranking_count == 0) return NULL;
    
    // Return the highest-ranked gateway
    IpfsGateway* best = rankings[0]->gateway;
    
    // Free rankings
    for (size_t i = 0; i < ranking_count; i++) {
        gateway_ranking_free(rankings[i]);
    }
    free(rankings);
    
    return best;
}

GatewayRanking** gateway_intelligence_rank_gateways(GatewayIntelligence* intelligence, 
                                                   size_t* ranking_count) {
    if (!intelligence || !ranking_count) return NULL;
    
    *ranking_count = 0;
    if (intelligence->gateway_count == 0) return NULL;
    
    GatewayRanking** rankings = calloc(intelligence->gateway_count, sizeof(GatewayRanking*));
    if (!rankings) return NULL;
    
    // Create rankings for each gateway
    for (size_t i = 0; i < intelligence->gateway_count; i++) {
        IpfsGateway* gateway = intelligence->discovered_gateways[i];
        GatewayMetrics* metrics = NULL;
        
        // Find corresponding metrics
        for (size_t j = 0; j < intelligence->metrics_count; j++) {
            if (intelligence->metrics[j] && 
                strcmp(intelligence->metrics[j]->gateway_url, gateway->url) == 0) {
                metrics = intelligence->metrics[j];
                break;
            }
        }
        
        if (!metrics || metrics->measurement_count < intelligence->min_measurements) {
            continue; // Skip gateways without sufficient measurements
        }
        
        GatewayRanking* ranking = gateway_ranking_create(gateway, metrics);
        if (ranking) {
            gateway_ranking_calculate_composite_score(ranking, 
                                                    intelligence->network_conditions,
                                                    intelligence);
            rankings[*ranking_count] = ranking;
            (*ranking_count)++;
        }
    }
    
    // Sort rankings by composite score (descending)
    for (size_t i = 0; i < *ranking_count - 1; i++) {
        for (size_t j = i + 1; j < *ranking_count; j++) {
            if (rankings[i]->composite_score < rankings[j]->composite_score) {
                GatewayRanking* temp = rankings[i];
                rankings[i] = rankings[j];
                rankings[j] = temp;
            }
        }
    }
    
    return rankings;
}

GatewayRanking* gateway_ranking_create(IpfsGateway* gateway, GatewayMetrics* metrics) {
    if (!gateway || !metrics) return NULL;
    
    GatewayRanking* ranking = xcalloc(1, sizeof(GatewayRanking));
    if (!ranking) return NULL;
    
    ranking->gateway = gateway;
    ranking->metrics = metrics;
    
    return ranking;
}

void gateway_ranking_free(GatewayRanking* ranking) {
    if (!ranking) return;
    
    free(ranking->recommendation_reason);
    free(ranking);
}

float gateway_ranking_calculate_composite_score(GatewayRanking* ranking, 
                                               const NetworkConditions* network_conditions,
                                               const GatewayIntelligence* intelligence) {
    if (!ranking || !intelligence) return 0.0f;
    
    // Calculate individual ranking components
    ranking->performance_rank = ranking->metrics->performance_score;
    ranking->reliability_rank = ranking->metrics->reliability_score;
    
    // Geographic ranking (simplified - would need actual geolocation)
    ranking->geographic_rank = 0.5f; // Default neutral score
    
    // Cost efficiency ranking (for paid gateways)
    ranking->cost_efficiency_rank = 1.0f; // Assume free gateways for now
    
    // Context-aware rankings
    if (network_conditions) {
        ranking->mobile_optimized_rank = network_conditions->is_mobile_network ? 
            (ranking->metrics->avg_download_speed > 1.0f ? 0.8f : 0.3f) : 0.5f;
        
        ranking->corporate_friendly_rank = network_conditions->behind_corporate_firewall ?
            (strstr(ranking->gateway->url, "https://") ? 0.8f : 0.2f) : 0.5f;
    } else {
        ranking->mobile_optimized_rank = 0.5f;
        ranking->corporate_friendly_rank = 0.5f;
    }
    
    // Privacy ranking (HTTPS vs HTTP)
    ranking->privacy_rank = strstr(ranking->gateway->url, "https://") ? 1.0f : 0.3f;
    
    // Calculate weighted composite score
    ranking->composite_score = 
        intelligence->performance_weight * ranking->performance_rank +
        intelligence->reliability_weight * ranking->reliability_rank +
        intelligence->geographic_weight * ranking->geographic_rank +
        intelligence->cost_weight * ranking->cost_efficiency_rank +
        intelligence->privacy_weight * ranking->privacy_rank;
    
    // Generate recommendation reason
    free(ranking->recommendation_reason);
    char reason[256];
    if (ranking->performance_rank > 0.8f) {
        snprintf(reason, sizeof(reason), "Excellent performance (%.1fms avg)", 
                ranking->metrics->avg_response_time);
    } else if (ranking->reliability_rank > 0.9f) {
        snprintf(reason, sizeof(reason), "Highly reliable (%.1f%% uptime)", 
                ranking->metrics->uptime_percentage * 100);
    } else {
        snprintf(reason, sizeof(reason), "Balanced performance and reliability");
    }
    ranking->recommendation_reason = strdup(reason);
    
    return ranking->composite_score;
}

bool gateway_intelligence_collect_metrics(GatewayIntelligence* intelligence) {
    if (!intelligence) return false;
    
    // Measure all gateways in parallel (simplified sequential version)
    for (size_t i = 0; i < intelligence->gateway_count; i++) {
        gateway_intelligence_measure_gateway(intelligence, intelligence->discovered_gateways[i]);
    }
    
    return true;
}

bool gateway_intelligence_learn_from_experience(GatewayIntelligence* intelligence, 
                                               const char* gateway_url,
                                               bool success, 
                                               float response_time, 
                                               size_t bytes_transferred) {
    if (!intelligence || !gateway_url) return false;
    
    // Find the gateway metrics
    for (size_t i = 0; i < intelligence->metrics_count; i++) {
        if (intelligence->metrics[i] && 
            strcmp(intelligence->metrics[i]->gateway_url, gateway_url) == 0) {
            return gateway_metrics_update(intelligence->metrics[i], 
                                        response_time, success, bytes_transferred);
        }
    }
    
    return false;
}