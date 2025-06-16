#include "package/p2p_discovery.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <json-c/json.h>
#include <curl/curl.h>

// Thread for background P2P operations
typedef struct {
    P2PDiscovery* discovery;
    pthread_t thread;
    bool should_stop;
} P2PThread;

static P2PThread* g_p2p_thread = NULL;
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Generate unique message ID
static char* generate_message_id(void) {
    static int counter = 0;
    char* id = malloc(32);
    if (!id) return NULL;
    
    snprintf(id, 32, "msg_%ld_%d", time(NULL), counter++);
    return id;
}

// Calculate distance between two geographic points
static float calculate_distance_km(float lat1, float lon1, float lat2, float lon2) {
    const float R = 6371.0f; // Earth's radius in km
    float dlat = (lat2 - lat1) * M_PI / 180.0f;
    float dlon = (lon2 - lon1) * M_PI / 180.0f;
    
    float a = sinf(dlat/2) * sinf(dlat/2) +
              cosf(lat1 * M_PI / 180.0f) * cosf(lat2 * M_PI / 180.0f) *
              sinf(dlon/2) * sinf(dlon/2);
    float c = 2 * atan2f(sqrtf(a), sqrtf(1-a));
    
    return R * c;
}

// Background thread for P2P operations
static void* p2p_background_thread(void* arg) {
    P2PThread* thread_data = (P2PThread*)arg;
    P2PDiscovery* discovery = thread_data->discovery;
    
    while (!thread_data->should_stop) {
        // Refresh peer list
        P2PPeer* peer = discovery->known_peers;
        time_t now = time(NULL);
        
        while (peer) {
            // Check peer liveness
            if (now - peer->last_seen > discovery->peer_timeout) {
                peer->consecutive_failures++;
            }
            peer = peer->next;
        }
        
        // Clean expired announcements
        P2PPackageAnnouncement* announcement = discovery->announcements;
        P2PPackageAnnouncement* prev = NULL;
        
        while (announcement) {
            if (now > announcement->expires_at) {
                P2PPackageAnnouncement* to_remove = announcement;
                if (prev) {
                    prev->next = announcement->next;
                } else {
                    discovery->announcements = announcement->next;
                }
                announcement = announcement->next;
                p2p_announcement_free(to_remove);
                discovery->announcement_count--;
            } else {
                prev = announcement;
                announcement = announcement->next;
            }
        }
        
        sleep(10); // Check every 10 seconds
    }
    
    return NULL;
}

P2PDiscovery* p2p_discovery_create(IpfsClient* ipfs_client, CryptoVerifier* verifier) {
    P2PDiscovery* discovery = calloc(1, sizeof(P2PDiscovery));
    if (!discovery) return NULL;
    
    discovery->ipfs_client = ipfs_client;
    discovery->crypto_verifier = verifier;
    
    // Default configuration
    discovery->auto_discovery_enabled = true;
    discovery->bandwidth_sharing_enabled = true;
    discovery->cache_sharing_enabled = true;
    discovery->sharing_ratio_target = 2.0f;
    
    discovery->min_peer_reputation = 0.3f;
    discovery->verify_all_packages = true;
    discovery->relay_enabled = true;
    
    discovery->max_concurrent_transfers = 5;
    discovery->announcement_ttl = 3600; // 1 hour
    discovery->peer_timeout = 300; // 5 minutes
    discovery->max_cache_size = 1024 * 1024 * 1024; // 1GB
    
    return discovery;
}

void p2p_discovery_free(P2PDiscovery* discovery) {
    if (!discovery) return;
    
    // Stop background thread
    p2p_discovery_stop(discovery);
    
    // Free peers
    P2PPeer* peer = discovery->known_peers;
    while (peer) {
        P2PPeer* next = peer->next;
        p2p_peer_free(peer);
        peer = next;
    }
    
    // Free trusted peers
    peer = discovery->trusted_peers;
    while (peer) {
        P2PPeer* next = peer->next;
        p2p_peer_free(peer);
        peer = next;
    }
    
    // Free blocked peers
    peer = discovery->blocked_peers;
    while (peer) {
        P2PPeer* next = peer->next;
        p2p_peer_free(peer);
        peer = next;
    }
    
    // Free swarms
    P2PSwarm* swarm = discovery->swarms;
    while (swarm) {
        P2PSwarm* next = swarm->next;
        free(swarm->topic);
        free(swarm->description);
        free(swarm->access_key);
        free(swarm->peers);
        free(swarm);
        swarm = next;
    }
    
    // Free announcements
    P2PPackageAnnouncement* announcement = discovery->announcements;
    while (announcement) {
        P2PPackageAnnouncement* next = announcement->next;
        p2p_announcement_free(announcement);
        announcement = next;
    }
    
    free(discovery);
}

bool p2p_discovery_initialize(P2PDiscovery* discovery) {
    if (!discovery) return false;
    
    // Initialize IPFS pubsub for P2P communication
    // This would connect to IPFS daemon's pubsub API
    
    // Join default swarms
    p2p_discovery_join_swarm(discovery, "goo-packages");
    p2p_discovery_join_swarm(discovery, "goo-discovery");
    
    return true;
}

bool p2p_discovery_start(P2PDiscovery* discovery) {
    if (!discovery || g_p2p_thread) return false;
    
    g_p2p_thread = calloc(1, sizeof(P2PThread));
    if (!g_p2p_thread) return false;
    
    g_p2p_thread->discovery = discovery;
    g_p2p_thread->should_stop = false;
    
    if (pthread_create(&g_p2p_thread->thread, NULL, p2p_background_thread, g_p2p_thread) != 0) {
        free(g_p2p_thread);
        g_p2p_thread = NULL;
        return false;
    }
    
    return true;
}

bool p2p_discovery_stop(P2PDiscovery* discovery) {
    if (!discovery || !g_p2p_thread) return false;
    
    g_p2p_thread->should_stop = true;
    pthread_join(g_p2p_thread->thread, NULL);
    
    free(g_p2p_thread);
    g_p2p_thread = NULL;
    
    return true;
}

bool p2p_discovery_add_peer(P2PDiscovery* discovery, const char* peer_id, 
                           const char* multiaddr) {
    if (!discovery || !peer_id || !multiaddr) return false;
    
    // Check if peer already exists
    P2PPeer* existing = p2p_discovery_get_peer(discovery, peer_id);
    if (existing) {
        // Update multiaddr if different
        if (strcmp(existing->multiaddr, multiaddr) != 0) {
            free(existing->multiaddr);
            existing->multiaddr = strdup(multiaddr);
        }
        return true;
    }
    
    // Create new peer
    P2PPeer* peer = p2p_peer_create(peer_id, multiaddr);
    if (!peer) return false;
    
    // Add to known peers list
    peer->next = discovery->known_peers;
    discovery->known_peers = peer;
    discovery->peer_count++;
    
    pthread_mutex_lock(&g_stats_mutex);
    discovery->total_packages_shared++; // Track discovery
    pthread_mutex_unlock(&g_stats_mutex);
    
    return true;
}

P2PPeer* p2p_discovery_get_peer(P2PDiscovery* discovery, const char* peer_id) {
    if (!discovery || !peer_id) return NULL;
    
    P2PPeer* peer = discovery->known_peers;
    while (peer) {
        if (strcmp(peer->peer_id, peer_id) == 0) {
            return peer;
        }
        peer = peer->next;
    }
    
    return NULL;
}

bool p2p_discovery_join_swarm(P2PDiscovery* discovery, const char* topic) {
    if (!discovery || !topic) return false;
    
    // Check if already in swarm
    P2PSwarm* swarm = discovery->swarms;
    while (swarm) {
        if (strcmp(swarm->topic, topic) == 0) {
            return true; // Already joined
        }
        swarm = swarm->next;
    }
    
    // Create new swarm
    swarm = p2p_discovery_create_swarm(discovery, topic);
    if (!swarm) return false;
    
    // Subscribe to IPFS pubsub topic
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    char url[512];
    snprintf(url, sizeof(url), "%s/api/v0/pubsub/sub?arg=%s", 
             discovery->ipfs_client->api_url, topic);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    return res == CURLE_OK;
}

P2PSwarm* p2p_discovery_create_swarm(P2PDiscovery* discovery, const char* topic) {
    if (!discovery || !topic) return NULL;
    
    P2PSwarm* swarm = calloc(1, sizeof(P2PSwarm));
    if (!swarm) return NULL;
    
    swarm->topic = strdup(topic);
    if (!swarm->topic) {
        free(swarm);
        return NULL;
    }
    
    swarm->peer_capacity = 32;
    swarm->peers = calloc(swarm->peer_capacity, sizeof(P2PPeer*));
    if (!swarm->peers) {
        free(swarm->topic);
        free(swarm);
        return NULL;
    }
    
    swarm->is_public = true;
    swarm->max_peers = 1000;
    swarm->created_at = time(NULL);
    swarm->last_activity = time(NULL);
    
    // Add to discovery's swarm list
    swarm->next = discovery->swarms;
    discovery->swarms = swarm;
    discovery->swarm_count++;
    
    return swarm;
}

bool p2p_discovery_announce_package(P2PDiscovery* discovery, const char* package_name,
                                   const char* version, const IpfsCid* cid) {
    if (!discovery || !package_name || !version || !cid) return false;
    
    // Create announcement
    P2PPackageAnnouncement* announcement = p2p_announcement_create(package_name, version, cid);
    if (!announcement) return false;
    
    announcement->announcer_peer_id = strdup(discovery->ipfs_client->peer_id);
    announcement->announced_at = time(NULL);
    announcement->expires_at = time(NULL) + discovery->announcement_ttl;
    
    // Add to local announcements
    announcement->next = discovery->announcements;
    discovery->announcements = announcement;
    discovery->announcement_count++;
    
    // Create announcement message
    json_object* msg = json_object_new_object();
    json_object_object_add(msg, "type", json_object_new_string("package_announcement"));
    json_object_object_add(msg, "package", json_object_new_string(package_name));
    json_object_object_add(msg, "version", json_object_new_string(version));
    json_object_object_add(msg, "cid", json_object_new_string(cid->hash));
    json_object_object_add(msg, "peer_id", json_object_new_string(discovery->ipfs_client->peer_id));
    json_object_object_add(msg, "timestamp", json_object_new_int64(time(NULL)));
    
    const char* json_str = json_object_to_json_string(msg);
    
    // Publish to relevant swarms
    bool success = p2p_discovery_gossip_publish(discovery, "goo-packages", json_str);
    
    json_object_put(msg);
    
    return success;
}

P2PSearchResult** p2p_discovery_search_packages(P2PDiscovery* discovery, 
                                               P2PSearchQuery* query,
                                               size_t* result_count) {
    if (!discovery || !query || !result_count) return NULL;
    
    *result_count = 0;
    
    // Count matching announcements
    P2PPackageAnnouncement* announcement = discovery->announcements;
    while (announcement) {
        // Check if announcement matches query
        bool matches = true;
        
        // Check query string match
        if (query->query_string) {
            if (!strstr(announcement->package_name, query->query_string) &&
                (!announcement->description || 
                 !strstr(announcement->description, query->query_string))) {
                matches = false;
            }
        }
        
        // Check version constraints
        if (matches && query->min_version) {
            // Simplified version comparison
            if (strcmp(announcement->version, query->min_version) < 0) {
                matches = false;
            }
        }
        
        // Check seeders
        if (matches && query->min_seeders > 0) {
            if (announcement->seeder_count < (size_t)query->min_seeders) {
                matches = false;
            }
        }
        
        // Check verification
        if (matches && query->verified_only && !announcement->is_verified) {
            matches = false;
        }
        
        if (matches) {
            (*result_count)++;
        }
        
        announcement = announcement->next;
    }
    
    if (*result_count == 0) return NULL;
    
    // Allocate results array
    P2PSearchResult** results = calloc(*result_count, sizeof(P2PSearchResult*));
    if (!results) return NULL;
    
    // Collect matching results
    size_t index = 0;
    announcement = discovery->announcements;
    
    while (announcement && index < *result_count) {
        // Re-check matches (simplified - would reuse logic)
        bool matches = true;
        
        if (query->query_string) {
            if (!strstr(announcement->package_name, query->query_string) &&
                (!announcement->description || 
                 !strstr(announcement->description, query->query_string))) {
                matches = false;
            }
        }
        
        if (matches) {
            P2PSearchResult* result = p2p_search_result_create(announcement);
            if (result) {
                // Calculate relevance score
                result->relevance_score = 1.0f;
                if (query->query_string) {
                    if (strstr(announcement->package_name, query->query_string)) {
                        result->relevance_score = 0.9f;
                    } else {
                        result->relevance_score = 0.5f;
                    }
                }
                
                // Adjust for availability
                result->relevance_score *= announcement->availability_score;
                
                // Set match reason
                result->match_reason = strdup("Package name match");
                
                // Find recommended peers
                if (announcement->seeder_count > 0) {
                    result->recommended_peer_count = 
                        (announcement->seeder_count > 3) ? 3 : announcement->seeder_count;
                    result->recommended_peers = calloc(result->recommended_peer_count, 
                                                     sizeof(P2PPeer*));
                    
                    // Select best peers based on reputation and latency
                    for (size_t i = 0; i < result->recommended_peer_count && 
                                        i < announcement->seeder_count; i++) {
                        result->recommended_peers[i] = announcement->seeders[i];
                    }
                }
                
                results[index++] = result;
            }
        }
        
        announcement = announcement->next;
    }
    
    // Sort results by relevance score (simple bubble sort)
    for (size_t i = 0; i < *result_count - 1; i++) {
        for (size_t j = i + 1; j < *result_count; j++) {
            if (results[i]->relevance_score < results[j]->relevance_score) {
                P2PSearchResult* temp = results[i];
                results[i] = results[j];
                results[j] = temp;
            }
        }
    }
    
    return results;
}

bool p2p_discovery_request_package(P2PDiscovery* discovery, const char* peer_id,
                                  const IpfsCid* cid) {
    if (!discovery || !peer_id || !cid) return false;
    
    P2PPeer* peer = p2p_discovery_get_peer(discovery, peer_id);
    if (!peer) return false;
    
    // Create package request message
    json_object* msg = json_object_new_object();
    json_object_object_add(msg, "type", json_object_new_string("package_request"));
    json_object_object_add(msg, "cid", json_object_new_string(cid->hash));
    json_object_object_add(msg, "requester", json_object_new_string(discovery->ipfs_client->peer_id));
    json_object_object_add(msg, "peer_id", json_object_new_string(peer_id));
    
    const char* json_str = json_object_to_json_string(msg);
    
    // Send direct message to peer
    // In practice, this would use IPFS's messaging or libp2p protocols
    bool success = true; // Simplified
    
    json_object_put(msg);
    
    if (success) {
        pthread_mutex_lock(&g_stats_mutex);
        discovery->total_packages_received++;
        pthread_mutex_unlock(&g_stats_mutex);
    }
    
    return success;
}

bool p2p_discovery_gossip_publish(P2PDiscovery* discovery, const char* topic,
                                 const char* message) {
    if (!discovery || !topic || !message) return false;
    
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    // Base64 encode the message (simplified - would use proper encoding)
    char* encoded = strdup(message);
    
    char url[1024];
    snprintf(url, sizeof(url), "%s/api/v0/pubsub/pub?arg=%s&arg=%s",
             discovery->ipfs_client->api_url, topic, encoded);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    free(encoded);
    
    return res == CURLE_OK;
}

// Peer operations
P2PPeer* p2p_peer_create(const char* peer_id, const char* multiaddr) {
    if (!peer_id || !multiaddr) return NULL;
    
    P2PPeer* peer = calloc(1, sizeof(P2PPeer));
    if (!peer) return NULL;
    
    peer->peer_id = strdup(peer_id);
    peer->multiaddr = strdup(multiaddr);
    
    if (!peer->peer_id || !peer->multiaddr) {
        p2p_peer_free(peer);
        return NULL;
    }
    
    // Default capabilities
    peer->supports_package_sharing = true;
    peer->supports_bandwidth_donation = true;
    peer->supports_cache_sharing = true;
    peer->supports_relay = false;
    
    // Default reputation
    peer->reputation_score = 0.5f;
    peer->upload_ratio = 1.0f;
    peer->last_seen = time(NULL);
    
    return peer;
}

void p2p_peer_free(P2PPeer* peer) {
    if (!peer) return;
    
    free(peer->peer_id);
    free(peer->multiaddr);
    free(peer->agent_version);
    free(peer->country_code);
    free(peer->region);
    free(peer);
}

bool p2p_peer_update_stats(P2PPeer* peer, bool success, size_t bytes_transferred) {
    if (!peer) return false;
    
    if (success) {
        peer->packages_shared++;
        peer->consecutive_failures = 0;
        peer->last_seen = time(NULL);
        
        // Update reputation positively
        peer->reputation_score += 0.01f;
        if (peer->reputation_score > 1.0f) {
            peer->reputation_score = 1.0f;
        }
    } else {
        peer->consecutive_failures++;
        
        // Update reputation negatively
        peer->reputation_score -= 0.05f;
        if (peer->reputation_score < 0.0f) {
            peer->reputation_score = 0.0f;
        }
    }
    
    return true;
}

float p2p_peer_calculate_reputation(const P2PPeer* peer) {
    if (!peer) return 0.0f;
    
    float reputation = peer->reputation_score;
    
    // Factor in upload ratio
    if (peer->upload_ratio > 2.0f) {
        reputation *= 1.1f;
    } else if (peer->upload_ratio < 0.5f) {
        reputation *= 0.9f;
    }
    
    // Factor in malicious reports
    if (peer->malicious_reports > 0) {
        reputation *= powf(0.8f, peer->malicious_reports);
    }
    
    // Factor in connection reliability
    if (peer->consecutive_failures > 3) {
        reputation *= 0.7f;
    }
    
    return fminf(fmaxf(reputation, 0.0f), 1.0f);
}

// Announcement operations
P2PPackageAnnouncement* p2p_announcement_create(const char* package_name, 
                                               const char* version,
                                               const IpfsCid* cid) {
    if (!package_name || !version || !cid) return NULL;
    
    P2PPackageAnnouncement* announcement = calloc(1, sizeof(P2PPackageAnnouncement));
    if (!announcement) return NULL;
    
    announcement->package_name = strdup(package_name);
    announcement->version = strdup(version);
    announcement->cid = ipfs_cid_clone(cid);
    
    if (!announcement->package_name || !announcement->version || !announcement->cid) {
        p2p_announcement_free(announcement);
        return NULL;
    }
    
    announcement->availability_score = 0.5f; // Default availability
    
    return announcement;
}

void p2p_announcement_free(P2PPackageAnnouncement* announcement) {
    if (!announcement) return;
    
    free(announcement->package_name);
    free(announcement->version);
    ipfs_cid_free(announcement->cid);
    free(announcement->announcer_peer_id);
    free(announcement->description);
    
    for (size_t i = 0; i < announcement->tag_count; i++) {
        free(announcement->tags[i]);
    }
    free(announcement->tags);
    
    free(announcement->seeders);
    
    for (size_t i = 0; i < announcement->signature_count; i++) {
        package_signature_free(announcement->signatures[i]);
    }
    free(announcement->signatures);
    
    free(announcement);
}

// Search operations
P2PSearchQuery* p2p_search_query_create(const char* query_string) {
    P2PSearchQuery* query = calloc(1, sizeof(P2PSearchQuery));
    if (!query) return NULL;
    
    if (query_string) {
        query->query_string = strdup(query_string);
        if (!query->query_string) {
            free(query);
            return NULL;
        }
    }
    
    // Default settings
    query->min_reputation = 0.3f;
    query->verified_only = false;
    query->max_results = 50;
    query->timeout_seconds = 30;
    query->parallel_search = true;
    
    return query;
}

void p2p_search_query_free(P2PSearchQuery* query) {
    if (!query) return;
    
    free(query->query_string);
    free(query->min_version);
    free(query->max_version);
    free(query->preferred_region);
    
    for (size_t i = 0; i < query->tag_count; i++) {
        free(query->tags[i]);
    }
    free(query->tags);
    
    free(query);
}

P2PSearchResult* p2p_search_result_create(P2PPackageAnnouncement* announcement) {
    if (!announcement) return NULL;
    
    P2PSearchResult* result = calloc(1, sizeof(P2PSearchResult));
    if (!result) return NULL;
    
    result->announcement = announcement;
    result->relevance_score = 0.5f;
    
    return result;
}

void p2p_search_result_free(P2PSearchResult* result) {
    if (!result) return;
    
    free(result->match_reason);
    free(result->recommended_peers);
    free(result);
}

// Statistics
P2PStats* p2p_discovery_get_stats(P2PDiscovery* discovery) {
    if (!discovery) return NULL;
    
    P2PStats* stats = calloc(1, sizeof(P2PStats));
    if (!stats) return NULL;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    stats->packages_uploaded = discovery->total_packages_shared;
    stats->packages_downloaded = discovery->total_packages_received;
    stats->bytes_uploaded = discovery->total_bytes_uploaded;
    stats->bytes_downloaded = discovery->total_bytes_downloaded;
    
    // Count peers by type
    P2PPeer* peer = discovery->known_peers;
    while (peer) {
        stats->total_peers_discovered++;
        if (time(NULL) - peer->last_seen < 300) {
            stats->active_peers++;
        }
        peer = peer->next;
    }
    
    peer = discovery->trusted_peers;
    while (peer) {
        stats->trusted_peers++;
        peer = peer->next;
    }
    
    peer = discovery->blocked_peers;
    while (peer) {
        stats->blocked_peers++;
        peer = peer->next;
    }
    
    stats->active_swarms = discovery->swarm_count;
    stats->total_announcements = discovery->announcement_count;
    
    // Calculate network health metrics
    if (stats->active_peers > 0) {
        float total_reputation = 0.0f;
        peer = discovery->known_peers;
        while (peer) {
            total_reputation += p2p_peer_calculate_reputation(peer);
            peer = peer->next;
        }
        stats->average_peer_reputation = total_reputation / stats->active_peers;
        stats->network_reliability = stats->average_peer_reputation;
    }
    
    stats->stats_period_start = time(NULL);
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    return stats;
}

void p2p_stats_free(P2PStats* stats) {
    free(stats);
}