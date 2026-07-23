#include "package/reputation_system.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <json-c/json.h>

// Thread safety
static pthread_mutex_t g_reputation_mutex = PTHREAD_MUTEX_INITIALIZER;

// Generate unique event ID
static char* generate_event_id(void) {
    static int counter = 0;
    char* id = malloc(32);
    if (!id) return NULL;
    
    snprintf(id, 32, "event_%ld_%d", time(NULL), counter++);
    return id;
}

// Calculate reputation decay based on time
static float calculate_time_decay(time_t event_time, time_t current_time, float decay_rate) {
    float days = (float)(current_time - event_time) / (24 * 3600);
    return expf(-decay_rate * days);
}

// Extract features for reputation prediction
static float* extract_reputation_features(const char* peer_id, ReputationSystem* system,
                                        size_t* feature_count) {
    *feature_count = 10; // Number of features
    float* features = calloc(*feature_count, sizeof(float));
    if (!features) return NULL;
    
    PeerReputation* rep = reputation_system_get_peer_reputation(system, peer_id);
    if (!rep) {
        free(features);
        return NULL;
    }
    
    // Feature 0: Current overall score
    features[0] = rep->score ? rep->score->overall_score : 0.5f;
    
    // Feature 1: Upload reliability
    features[1] = rep->upload_reliability;
    
    // Feature 2: Download behavior
    features[2] = rep->download_behavior;
    
    // Feature 3: Bandwidth sharing ratio
    features[3] = rep->bandwidth_sharing_ratio;
    
    // Feature 4: Response time score
    features[4] = rep->response_time_score;
    
    // Feature 5: Positive interaction ratio
    int total_interactions = rep->positive_interactions + rep->negative_interactions;
    features[5] = total_interactions > 0 ? 
                 (float)rep->positive_interactions / total_interactions : 0.5f;
    
    // Feature 6: Reports received (normalized)
    features[6] = fminf((float)rep->reports_received / 10.0f, 1.0f);
    
    // Feature 7: Account age (days, normalized)
    time_t now = time(NULL);
    if (rep->score && rep->score->score_period_start > 0) {
        float days = (float)(now - rep->score->score_period_start) / (24 * 3600);
        features[7] = fminf(days / 365.0f, 1.0f); // Normalize to 1 year
    } else {
        features[7] = 0.0f;
    }
    
    // Feature 8: Event count (normalized)
    features[8] = fminf((float)rep->event_count / 100.0f, 1.0f);
    
    // Feature 9: Verification status
    features[9] = rep->is_verified_peer ? 1.0f : 0.0f;
    
    return features;
}

ReputationSystem* reputation_system_create(void) {
    ReputationSystem* system = xcalloc(1, sizeof(ReputationSystem));
    if (!system) return NULL;
    
    // Default configuration
    system->min_reputation_threshold = 0.3f;
    system->trust_decay_rate = 0.01f; // 1% decay per day
    system->reputation_window_days = 90;
    system->enable_trust_network = true;
    
    // Default scoring weights
    system->reliability_weight = 0.3f;
    system->security_weight = 0.3f;
    system->performance_weight = 0.2f;
    system->community_weight = 0.2f;
    
    // Verification settings
    system->require_event_verification = true;
    system->min_verifiers = 2;
    system->verification_threshold = 0.7f;
    
    // Anti-gaming measures
    system->enable_sybil_detection = true;
    system->enable_collusion_detection = true;
    system->max_self_boost = 0.1f;
    
    return system;
}

void reputation_system_free(ReputationSystem* system) {
    if (!system) return;
    
    // Free peer reputations
    PeerReputation* peer_rep = system->peer_reputations;
    while (peer_rep) {
        PeerReputation* next = peer_rep->next;
        peer_reputation_free(peer_rep);
        peer_rep = next;
    }
    
    // Free package reputations
    PackageReputation* pkg_rep = system->package_reputations;
    while (pkg_rep) {
        PackageReputation* next = pkg_rep->next;
        package_reputation_free(pkg_rep);
        pkg_rep = next;
    }
    
    // Free events
    ReputationEvent* event = system->events;
    while (event) {
        ReputationEvent* next = event->next;
        reputation_event_free(event);
        event = next;
    }
    
    free(system);
}

bool reputation_system_initialize(ReputationSystem* system) {
    if (!system) return false;
    
    // Initialize statistics
    system->total_events = 0;
    system->total_peers = 0;
    system->total_packages = 0;
    system->average_network_reputation = 0.5f;
    
    return true;
}

bool reputation_system_report_event(ReputationSystem* system,
                                   ReputationEventType type,
                                   const char* subject_id,
                                   const char* reporter_id,
                                   float severity,
                                   const char* description) {
    if (!system || !subject_id || !reporter_id) return false;
    
    pthread_mutex_lock(&g_reputation_mutex);
    
    // Create new event
    ReputationEvent* event = reputation_event_create(type, subject_id, reporter_id);
    if (!event) {
        pthread_mutex_unlock(&g_reputation_mutex);
        return false;
    }
    
    event->severity = fmaxf(0.0f, fminf(severity, 1.0f));
    if (description) {
        event->description = strdup(description);
    }
    
    // Calculate reputation impact based on event type
    switch (type) {
        case EVENT_SUCCESSFUL_DOWNLOAD:
            event->reputation_impact = 0.05f * severity;
            event->dimension = REPUTATION_RELIABILITY;
            break;
        case EVENT_FAILED_DOWNLOAD:
            event->reputation_impact = -0.1f * severity;
            event->dimension = REPUTATION_RELIABILITY;
            break;
        case EVENT_MALICIOUS_CONTENT:
            event->reputation_impact = -0.5f * severity;
            event->dimension = REPUTATION_SECURITY;
            break;
        case EVENT_INVALID_SIGNATURE:
            event->reputation_impact = -0.3f * severity;
            event->dimension = REPUTATION_SECURITY;
            break;
        case EVENT_GOOD_BANDWIDTH_SHARING:
            event->reputation_impact = 0.1f * severity;
            event->dimension = REPUTATION_COMMUNITY;
            break;
        case EVENT_POOR_BANDWIDTH_SHARING:
            event->reputation_impact = -0.15f * severity;
            event->dimension = REPUTATION_COMMUNITY;
            break;
        case EVENT_HELPFUL_PEER:
            event->reputation_impact = 0.2f * severity;
            event->dimension = REPUTATION_COMMUNITY;
            break;
        case EVENT_SPAM_BEHAVIOR:
            event->reputation_impact = -0.25f * severity;
            event->dimension = REPUTATION_COMMUNITY;
            break;
        case EVENT_VULNERABILITY_REPORT:
            event->reputation_impact = -0.4f * severity;
            event->dimension = REPUTATION_SECURITY;
            break;
        case EVENT_SECURITY_CONTRIBUTION:
            event->reputation_impact = 0.3f * severity;
            event->dimension = REPUTATION_SECURITY;
            break;
        case EVENT_COMMUNITY_PARTICIPATION:
            event->reputation_impact = 0.1f * severity;
            event->dimension = REPUTATION_COMMUNITY;
            break;
        default:
            event->reputation_impact = 0.0f;
            event->dimension = REPUTATION_TRUSTWORTHINESS;
            break;
    }
    
    // Add event to system
    event->next = system->events;
    system->events = event;
    system->total_events++;
    
    // If verification is not required, process immediately
    if (!system->require_event_verification) {
        event->is_verified = true;
        event->verification_confidence = 1.0f;
        
        // Update reputation
        reputation_system_update_peer_reputation(system, subject_id);
    }
    
    pthread_mutex_unlock(&g_reputation_mutex);
    
    return true;
}

bool reputation_system_verify_event(ReputationSystem* system,
                                   const char* event_id,
                                   const char* verifier_id) {
    if (!system || !event_id || !verifier_id) return false;
    
    pthread_mutex_lock(&g_reputation_mutex);
    
    // Find event
    ReputationEvent* event = system->events;
    while (event) {
        if (strcmp(event->event_id, event_id) == 0) {
            break;
        }
        event = event->next;
    }
    
    if (!event) {
        pthread_mutex_unlock(&g_reputation_mutex);
        return false;
    }
    
    // Check if verifier already verified this event
    for (size_t i = 0; i < event->verifier_count; i++) {
        if (strcmp(event->verifier_ids[i], verifier_id) == 0) {
            pthread_mutex_unlock(&g_reputation_mutex);
            return false; // Already verified by this peer
        }
    }
    
    // Add verifier
    char** new_verifiers = realloc(event->verifier_ids, 
                                  (event->verifier_count + 1) * sizeof(char*));
    if (!new_verifiers) {
        pthread_mutex_unlock(&g_reputation_mutex);
        return false;
    }
    
    event->verifier_ids = new_verifiers;
    event->verifier_ids[event->verifier_count] = strdup(verifier_id);
    event->verifier_count++;
    
    // Update verification confidence
    event->verification_confidence = (float)event->verifier_count / system->min_verifiers;
    
    // Mark as verified if threshold is met
    if (event->verifier_count >= system->min_verifiers &&
        event->verification_confidence >= system->verification_threshold) {
        event->is_verified = true;
        
        // Update reputation now that event is verified
        reputation_system_update_peer_reputation(system, event->subject_id);
    }
    
    pthread_mutex_unlock(&g_reputation_mutex);
    
    return true;
}

bool reputation_system_update_peer_reputation(ReputationSystem* system,
                                             const char* peer_id) {
    if (!system || !peer_id) return false;
    
    pthread_mutex_lock(&g_reputation_mutex);
    
    // Find or create peer reputation
    PeerReputation* rep = reputation_system_get_peer_reputation(system, peer_id);
    if (!rep) {
        rep = peer_reputation_create(peer_id);
        if (!rep) {
            pthread_mutex_unlock(&g_reputation_mutex);
            return false;
        }
        rep->next = system->peer_reputations;
        system->peer_reputations = rep;
        system->total_peers++;
    }
    
    // Collect all verified events for this peer
    time_t now = time(NULL);
    time_t window_start = now - (system->reputation_window_days * 24 * 3600);
    
    ReputationEvent* event = system->events;
    float reputation_deltas[5] = {0.0f}; // One for each dimension
    int event_count = 0;
    
    while (event) {
        if (strcmp(event->subject_id, peer_id) == 0 &&
            event->is_verified &&
            event->timestamp > window_start) {
            
            // Apply time decay
            float decay = calculate_time_decay(event->timestamp, now, system->trust_decay_rate);
            float impact = event->reputation_impact * decay;
            
            // Add to appropriate dimension
            switch (event->dimension) {
                case REPUTATION_RELIABILITY:
                    reputation_deltas[0] += impact;
                    break;
                case REPUTATION_SECURITY:
                    reputation_deltas[1] += impact;
                    break;
                case REPUTATION_PERFORMANCE:
                    reputation_deltas[2] += impact;
                    break;
                case REPUTATION_COMMUNITY:
                    reputation_deltas[3] += impact;
                    break;
                case REPUTATION_TRUSTWORTHINESS:
                    reputation_deltas[4] += impact;
                    break;
            }
            
            event_count++;
        }
        event = event->next;
    }
    
    // Update reputation scores
    if (!rep->score) {
        rep->score = reputation_score_create();
    }
    
    if (rep->score) {
        // Apply deltas with bounds checking
        rep->score->reliability = fmaxf(0.0f, fminf(1.0f, 
            rep->score->reliability + reputation_deltas[0]));
        rep->score->security = fmaxf(0.0f, fminf(1.0f, 
            rep->score->security + reputation_deltas[1]));
        rep->score->performance = fmaxf(0.0f, fminf(1.0f, 
            rep->score->performance + reputation_deltas[2]));
        rep->score->community = fmaxf(0.0f, fminf(1.0f, 
            rep->score->community + reputation_deltas[3]));
        rep->score->trustworthiness = fmaxf(0.0f, fminf(1.0f, 
            rep->score->trustworthiness + reputation_deltas[4]));
        
        // Calculate overall score as weighted average
        rep->score->overall_score = 
            system->reliability_weight * rep->score->reliability +
            system->security_weight * rep->score->security +
            system->performance_weight * rep->score->performance +
            system->community_weight * rep->score->community;
        
        // Update confidence based on event count
        rep->score->confidence = fminf(1.0f, (float)event_count / 10.0f);
        rep->score->event_count = event_count;
        rep->score->last_updated = now;
    }
    
    rep->event_count = event_count;
    
    pthread_mutex_unlock(&g_reputation_mutex);
    
    return true;
}

PeerReputation* reputation_system_get_peer_reputation(ReputationSystem* system,
                                                     const char* peer_id) {
    if (!system || !peer_id) return NULL;
    
    PeerReputation* rep = system->peer_reputations;
    while (rep) {
        if (strcmp(rep->peer_id, peer_id) == 0) {
            return rep;
        }
        rep = rep->next;
    }
    
    return NULL;
}

float reputation_system_get_trust_score(ReputationSystem* system,
                                       const char* peer_id) {
    if (!system || !peer_id) return 0.0f;
    
    PeerReputation* rep = reputation_system_get_peer_reputation(system, peer_id);
    if (!rep || !rep->score) return 0.5f; // Default neutral score
    
    return rep->score->overall_score;
}

bool reputation_system_detect_sybil_attack(ReputationSystem* system,
                                          const char* peer_id) {
    if (!system || !peer_id) return false;
    
    // Simple Sybil detection heuristics
    PeerReputation* rep = reputation_system_get_peer_reputation(system, peer_id);
    if (!rep) return false;
    
    time_t now = time(NULL);
    
    // Check for new account with high activity
    if (rep->score && rep->score->score_period_start > 0) {
        float account_age_days = (float)(now - rep->score->score_period_start) / (24 * 3600);
        
        // Suspicious if account is very new but has many interactions
        if (account_age_days < 7 && rep->event_count > 50) {
            return true;
        }
    }
    
    // Check for unusual patterns
    int positive_reports = 0;
    int total_reports = 0;
    
    ReputationEvent* event = system->events;
    while (event) {
        if (strcmp(event->reporter_id, peer_id) == 0) {
            total_reports++;
            if (event->reputation_impact > 0) {
                positive_reports++;
            }
        }
        event = event->next;
    }
    
    // Suspicious if peer only reports positive events (potential fake reviews)
    if (total_reports > 10 && positive_reports == total_reports) {
        return true;
    }
    
    return false;
}

char** reputation_system_filter_trusted_peers(ReputationSystem* system,
                                             char** peer_ids,
                                             size_t peer_count,
                                             float min_reputation,
                                             size_t* filtered_count) {
    if (!system || !peer_ids || !filtered_count) return NULL;
    
    *filtered_count = 0;
    char** filtered = malloc(peer_count * sizeof(char*));
    if (!filtered) return NULL;
    
    for (size_t i = 0; i < peer_count; i++) {
        float trust_score = reputation_system_get_trust_score(system, peer_ids[i]);
        
        if (trust_score >= min_reputation &&
            !reputation_system_detect_sybil_attack(system, peer_ids[i])) {
            filtered[*filtered_count] = strdup(peer_ids[i]);
            (*filtered_count)++;
        }
    }
    
    return filtered;
}

// Reputation operations
PeerReputation* peer_reputation_create(const char* peer_id) {
    if (!peer_id) return NULL;
    
    PeerReputation* rep = xcalloc(1, sizeof(PeerReputation));
    if (!rep) return NULL;
    
    rep->peer_id = strdup(peer_id);
    if (!rep->peer_id) {
        free(rep);
        return NULL;
    }
    
    rep->score = reputation_score_create();
    if (!rep->score) {
        peer_reputation_free(rep);
        return NULL;
    }
    
    // Initialize with neutral values
    rep->upload_reliability = 0.5f;
    rep->download_behavior = 0.5f;
    rep->bandwidth_sharing_ratio = 1.0f;
    rep->response_time_score = 0.5f;
    
    return rep;
}

void peer_reputation_free(PeerReputation* reputation) {
    if (!reputation) return;
    
    free(reputation->peer_id);
    reputation_score_free(reputation->score);
    free(reputation->verification_method);
    
    // Free score history
    for (size_t i = 0; i < reputation->score_history_size; i++) {
        reputation_score_free(reputation->score_history[i]);
    }
    free(reputation->score_history);
    
    // Free trust relationships
    TrustRelationship* trust = reputation->trust_relationships;
    while (trust) {
        TrustRelationship* next = trust->next;
        trust_relationship_free(trust);
        trust = next;
    }
    
    free(reputation);
}

ReputationEvent* reputation_event_create(ReputationEventType type,
                                        const char* subject_id,
                                        const char* reporter_id) {
    if (!subject_id || !reporter_id) return NULL;
    
    ReputationEvent* event = xcalloc(1, sizeof(ReputationEvent));
    if (!event) return NULL;
    
    event->event_id = generate_event_id();
    event->subject_id = strdup(subject_id);
    event->reporter_id = strdup(reporter_id);
    
    if (!event->event_id || !event->subject_id || !event->reporter_id) {
        reputation_event_free(event);
        return NULL;
    }
    
    event->type = type;
    event->timestamp = time(NULL);
    event->severity = 1.0f;
    event->verification_confidence = 0.0f;
    
    return event;
}

void reputation_event_free(ReputationEvent* event) {
    if (!event) return;
    
    free(event->event_id);
    free(event->subject_id);
    free(event->reporter_id);
    free(event->description);
    free(event->evidence_hash);
    
    for (size_t i = 0; i < event->verifier_count; i++) {
        free(event->verifier_ids[i]);
    }
    free(event->verifier_ids);
    
    free(event);
}

ReputationScore* reputation_score_create(void) {
    ReputationScore* score = xcalloc(1, sizeof(ReputationScore));
    if (!score) return NULL;
    
    // Initialize with neutral scores
    score->reliability = 0.5f;
    score->security = 0.5f;
    score->performance = 0.5f;
    score->community = 0.5f;
    score->trustworthiness = 0.5f;
    score->overall_score = 0.5f;
    score->confidence = 0.0f;
    score->last_updated = time(NULL);
    score->score_period_start = time(NULL);
    
    return score;
}

void reputation_score_free(ReputationScore* score) {
    free(score);
}

// Statistics
ReputationStats* reputation_system_get_stats(ReputationSystem* system) {
    if (!system) return NULL;
    
    ReputationStats* stats = xcalloc(1, sizeof(ReputationStats));
    if (!stats) return NULL;
    
    pthread_mutex_lock(&g_reputation_mutex);
    
    // Calculate network statistics
    float total_reputation = 0.0f;
    size_t reputation_count = 0;
    size_t high_rep_peers = 0;
    size_t low_rep_peers = 0;
    
    PeerReputation* rep = system->peer_reputations;
    while (rep) {
        if (rep->score) {
            total_reputation += rep->score->overall_score;
            reputation_count++;
            
            if (rep->score->overall_score > 0.7f) {
                high_rep_peers++;
            } else if (rep->score->overall_score < 0.3f) {
                low_rep_peers++;
            }
        }
        rep = rep->next;
    }
    
    stats->average_reputation = reputation_count > 0 ? 
                               total_reputation / reputation_count : 0.5f;
    stats->high_reputation_peers = high_rep_peers;
    stats->low_reputation_peers = low_rep_peers;
    
    // Event statistics
    stats->total_events = system->total_events;
    
    size_t verified_events = 0;
    ReputationEvent* event = system->events;
    while (event) {
        if (event->is_verified) {
            verified_events++;
        }
        event = event->next;
    }
    stats->verified_events = verified_events;
    
    // Trust network statistics
    size_t trust_relationships = 0;
    float total_trust = 0.0f;
    
    rep = system->peer_reputations;
    while (rep) {
        TrustRelationship* trust = rep->trust_relationships;
        while (trust) {
            trust_relationships++;
            total_trust += trust->trust_level;
            trust = trust->next;
        }
        rep = rep->next;
    }
    
    stats->trust_relationships = trust_relationships;
    stats->average_trust_level = trust_relationships > 0 ?
                                total_trust / trust_relationships : 0.5f;
    
    stats->stats_generated_at = time(NULL);
    
    pthread_mutex_unlock(&g_reputation_mutex);
    
    return stats;
}

void reputation_stats_free(ReputationStats* stats) {
    free(stats);
}