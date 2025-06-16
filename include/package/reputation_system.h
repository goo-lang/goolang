#ifndef REPUTATION_SYSTEM_H
#define REPUTATION_SYSTEM_H

#include "p2p_discovery.h"
#include "crypto_verifier.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Forward declarations
typedef struct ReputationSystem ReputationSystem;
typedef struct PeerReputation PeerReputation;
typedef struct PackageReputation PackageReputation;
typedef struct ReputationScore ReputationScore;
typedef struct ReputationEvent ReputationEvent;
typedef struct TrustRelationship TrustRelationship;

// Reputation event types
typedef enum {
    EVENT_SUCCESSFUL_DOWNLOAD,      // Successful package download
    EVENT_FAILED_DOWNLOAD,          // Failed package download
    EVENT_MALICIOUS_CONTENT,        // Reported malicious content
    EVENT_INVALID_SIGNATURE,        // Invalid package signature
    EVENT_GOOD_BANDWIDTH_SHARING,   // Good bandwidth sharing behavior
    EVENT_POOR_BANDWIDTH_SHARING,   // Poor bandwidth sharing behavior
    EVENT_HELPFUL_PEER,             // Helpful peer behavior
    EVENT_SPAM_BEHAVIOR,            // Spam or flooding behavior
    EVENT_VULNERABILITY_REPORT,     // Reported package vulnerability
    EVENT_SECURITY_CONTRIBUTION,    // Security contribution
    EVENT_COMMUNITY_PARTICIPATION   // Active community participation
} ReputationEventType;

// Reputation dimensions
typedef enum {
    REPUTATION_RELIABILITY,         // How reliable is the peer/package
    REPUTATION_SECURITY,           // Security-related reputation
    REPUTATION_PERFORMANCE,        // Performance and quality
    REPUTATION_COMMUNITY,          // Community contribution
    REPUTATION_TRUSTWORTHINESS     // Overall trustworthiness
} ReputationDimension;

// Individual reputation event
typedef struct ReputationEvent {
    char* event_id;                 // Unique event identifier
    ReputationEventType type;       // Event type
    
    // Event participants
    char* subject_id;               // Who/what is being rated
    char* reporter_id;              // Who reported this event
    char* witness_ids[10];          // Witnesses to the event
    size_t witness_count;
    
    // Event details
    time_t timestamp;               // When event occurred
    float severity;                 // Event severity (0.0-1.0)
    char* description;              // Event description
    char* evidence_hash;            // Hash of evidence data
    
    // Verification
    bool is_verified;               // Has been verified
    float verification_confidence;  // Confidence in verification
    char** verifier_ids;            // Who verified this event
    size_t verifier_count;
    
    // Impact on reputation
    float reputation_impact;        // How much this affects reputation
    ReputationDimension dimension;  // Which dimension is affected
    
    struct ReputationEvent* next;   // Linked list
} ReputationEvent;

// Multi-dimensional reputation score
typedef struct ReputationScore {
    // Core dimensions
    float reliability;              // Reliability score (0.0-1.0)
    float security;                 // Security score (0.0-1.0)
    float performance;              // Performance score (0.0-1.0)
    float community;                // Community score (0.0-1.0)
    float trustworthiness;          // Overall trust (0.0-1.0)
    
    // Composite scores
    float overall_score;            // Weighted overall score
    float confidence;               // Confidence in scores (0.0-1.0)
    
    // Score metadata
    time_t last_updated;            // Last score update
    int event_count;                // Number of events considered
    time_t score_period_start;      // Score calculation period start
    
    // Trend information
    float trend_direction;          // Score trend (-1.0 to 1.0)
    float volatility;               // Score volatility
} ReputationScore;

// Peer reputation tracking
typedef struct PeerReputation {
    char* peer_id;                  // Peer identifier
    ReputationScore* score;         // Current reputation score
    
    // Historical data
    ReputationEvent* events;        // Events affecting this peer
    size_t event_count;
    ReputationScore** score_history; // Historical scores
    size_t score_history_size;
    
    // Peer behavior patterns
    float upload_reliability;       // Upload success rate
    float download_behavior;        // Download behavior score
    float bandwidth_sharing_ratio;  // Upload/download ratio
    float response_time_score;      // Response time quality
    
    // Community interactions
    int positive_interactions;      // Positive interactions count
    int negative_interactions;      // Negative interactions count
    int reports_received;           // Reports against this peer
    int reports_made;               // Reports made by this peer
    
    // Trust network
    TrustRelationship* trust_relationships;
    size_t trust_relationship_count;
    
    // Verification status
    bool is_verified_peer;          // Verified identity
    char* verification_method;      // How identity was verified
    time_t verified_at;             // When verification occurred
    
    struct PeerReputation* next;    // Linked list
} PeerReputation;

// Package reputation tracking
typedef struct PackageReputation {
    char* package_name;             // Package name
    char* package_version;          // Package version
    char* content_hash;             // Package content hash
    
    ReputationScore* score;         // Package reputation score
    
    // Package quality metrics
    float security_score;           // Security assessment
    float code_quality_score;       // Code quality assessment
    float documentation_score;      // Documentation quality
    float test_coverage_score;      // Test coverage assessment
    
    // Usage statistics
    int download_count;             // Total downloads
    int successful_downloads;       // Successful downloads
    int failed_downloads;           // Failed downloads
    int vulnerability_reports;      // Vulnerability reports
    
    // Community feedback
    int positive_reviews;           // Positive reviews
    int negative_reviews;           // Negative reviews
    float average_rating;           // Average user rating
    
    // Maintainer reputation
    char** maintainer_ids;          // Package maintainers
    size_t maintainer_count;
    float maintainer_reputation;    // Average maintainer reputation
    
    // Dependency analysis
    int dependency_count;           // Number of dependencies
    float dependency_reputation;    // Average dependency reputation
    bool has_security_audit;        // Has security audit
    time_t last_audit_date;         // Last security audit date
    
    struct PackageReputation* next; // Linked list
} PackageReputation;

// Trust relationship between peers
typedef struct TrustRelationship {
    char* peer_id;                  // Trusted peer ID
    float trust_level;              // Trust level (0.0-1.0)
    time_t established_at;          // When trust was established
    time_t last_updated;            // Last trust update
    
    // Trust basis
    char* trust_reason;             // Why this peer is trusted
    int positive_interactions;      // Positive interactions
    int total_interactions;         // Total interactions
    
    // Trust verification
    bool is_mutual;                 // Is trust mutual
    bool is_verified;               // Is trust verified
    char** verifying_peers;         // Peers who verify this trust
    size_t verifier_count;
    
    struct TrustRelationship* next; // Linked list
} TrustRelationship;

// Reputation system manager
typedef struct ReputationSystem {
    // Reputation data
    PeerReputation* peer_reputations;
    PackageReputation* package_reputations;
    ReputationEvent* events;
    
    // System configuration
    float min_reputation_threshold; // Minimum reputation for interaction
    float trust_decay_rate;         // How fast trust decays over time
    int reputation_window_days;     // Reputation calculation window
    bool enable_trust_network;      // Enable trust network propagation
    
    // Scoring weights
    float reliability_weight;       // Weight for reliability dimension
    float security_weight;          // Weight for security dimension
    float performance_weight;       // Weight for performance dimension
    float community_weight;         // Weight for community dimension
    
    // Verification settings
    bool require_event_verification; // Require event verification
    int min_verifiers;              // Minimum number of verifiers
    float verification_threshold;   // Verification confidence threshold
    
    // Anti-gaming measures
    bool enable_sybil_detection;    // Enable Sybil attack detection
    bool enable_collusion_detection; // Enable collusion detection
    float max_self_boost;           // Maximum self-boosting allowed
    
    // Integration components
    P2PDiscovery* p2p_discovery;
    CryptoVerifier* crypto_verifier;
    
    // Statistics
    size_t total_events;
    size_t total_peers;
    size_t total_packages;
    float average_network_reputation;
} ReputationSystem;

// Function declarations

// Reputation System lifecycle
ReputationSystem* reputation_system_create(void);
void reputation_system_free(ReputationSystem* system);
bool reputation_system_initialize(ReputationSystem* system);

// Event reporting and processing
bool reputation_system_report_event(ReputationSystem* system,
                                   ReputationEventType type,
                                   const char* subject_id,
                                   const char* reporter_id,
                                   float severity,
                                   const char* description);
bool reputation_system_verify_event(ReputationSystem* system,
                                   const char* event_id,
                                   const char* verifier_id);
bool reputation_system_process_events(ReputationSystem* system);

// Reputation calculation
bool reputation_system_update_peer_reputation(ReputationSystem* system,
                                             const char* peer_id);
bool reputation_system_update_package_reputation(ReputationSystem* system,
                                                const char* package_name,
                                                const char* version);
ReputationScore* reputation_system_calculate_score(ReputationSystem* system,
                                                  const char* subject_id,
                                                  bool is_peer);

// Reputation queries
PeerReputation* reputation_system_get_peer_reputation(ReputationSystem* system,
                                                     const char* peer_id);
PackageReputation* reputation_system_get_package_reputation(ReputationSystem* system,
                                                           const char* package_name,
                                                           const char* version);
float reputation_system_get_trust_score(ReputationSystem* system,
                                       const char* peer_id);

// Trust network operations
bool reputation_system_establish_trust(ReputationSystem* system,
                                      const char* peer_id1,
                                      const char* peer_id2,
                                      float trust_level);
bool reputation_system_verify_trust(ReputationSystem* system,
                                   const char* peer_id1,
                                   const char* peer_id2,
                                   const char* verifier_id);
float reputation_system_calculate_transitive_trust(ReputationSystem* system,
                                                  const char* from_peer,
                                                  const char* to_peer);

// Anti-gaming and security
bool reputation_system_detect_sybil_attack(ReputationSystem* system,
                                          const char* peer_id);
bool reputation_system_detect_collusion(ReputationSystem* system,
                                       const char** peer_ids,
                                       size_t peer_count);
bool reputation_system_validate_event(ReputationSystem* system,
                                     ReputationEvent* event);

// Peer filtering and ranking
char** reputation_system_filter_trusted_peers(ReputationSystem* system,
                                             char** peer_ids,
                                             size_t peer_count,
                                             float min_reputation,
                                             size_t* filtered_count);
char** reputation_system_rank_peers_by_reputation(ReputationSystem* system,
                                                 char** peer_ids,
                                                 size_t peer_count);

// Package quality assessment
bool reputation_system_assess_package_quality(ReputationSystem* system,
                                             const char* package_name,
                                             const char* version,
                                             const char* package_data);
bool reputation_system_security_audit_package(ReputationSystem* system,
                                             const char* package_name,
                                             const char* version);

// Reputation operations
PeerReputation* peer_reputation_create(const char* peer_id);
void peer_reputation_free(PeerReputation* reputation);
bool peer_reputation_add_event(PeerReputation* reputation, ReputationEvent* event);
float peer_reputation_calculate_overall_score(const PeerReputation* reputation);

PackageReputation* package_reputation_create(const char* package_name,
                                            const char* version);
void package_reputation_free(PackageReputation* reputation);
bool package_reputation_update_metrics(PackageReputation* reputation,
                                      bool download_success,
                                      float quality_score);

ReputationEvent* reputation_event_create(ReputationEventType type,
                                        const char* subject_id,
                                        const char* reporter_id);
void reputation_event_free(ReputationEvent* event);
bool reputation_event_add_evidence(ReputationEvent* event, const char* evidence);

ReputationScore* reputation_score_create(void);
void reputation_score_free(ReputationScore* score);
bool reputation_score_update(ReputationScore* score,
                            ReputationDimension dimension,
                            float delta);

// Trust relationship operations
TrustRelationship* trust_relationship_create(const char* peer_id, float trust_level);
void trust_relationship_free(TrustRelationship* relationship);
bool trust_relationship_update(TrustRelationship* relationship,
                             bool positive_interaction);

// Advanced reputation algorithms

// PageRank-style reputation propagation
typedef struct ReputationGraph {
    char** peer_ids;                // Peer identifiers
    size_t peer_count;
    float** adjacency_matrix;       // Trust relationships
    float* reputation_vector;       // Current reputation scores
    float damping_factor;           // PageRank damping factor
} ReputationGraph;

ReputationGraph* reputation_graph_create(ReputationSystem* system);
bool reputation_graph_run_pagerank(ReputationGraph* graph, int iterations);
void reputation_graph_free(ReputationGraph* graph);

// Temporal reputation analysis
typedef struct TemporalReputation {
    time_t* timestamps;             // Time points
    float* scores;                  // Scores at each time point
    size_t point_count;
    
    // Trend analysis
    float slope;                    // Reputation trend slope
    float volatility;               // Score volatility
    float seasonal_factor;          // Seasonal adjustment
} TemporalReputation;

TemporalReputation* temporal_reputation_analyze(ReputationSystem* system,
                                              const char* subject_id,
                                              int days_back);
void temporal_reputation_free(TemporalReputation* temporal);

// Machine learning for reputation prediction
typedef struct ReputationPredictor {
    // Model parameters
    float* weights;                 // Model weights
    size_t weight_count;
    float bias;                     // Model bias
    
    // Training data
    float** training_features;      // Feature vectors
    float* training_labels;         // Reputation scores
    size_t training_size;
    
    // Model performance
    float accuracy;                 // Prediction accuracy
    float mse;                      // Mean squared error
} ReputationPredictor;

ReputationPredictor* reputation_predictor_create(void);
bool reputation_predictor_train(ReputationPredictor* predictor,
                               ReputationSystem* system);
float reputation_predictor_predict(ReputationPredictor* predictor,
                                 const char* peer_id,
                                 ReputationSystem* system);
void reputation_predictor_free(ReputationPredictor* predictor);

// Statistics and monitoring
typedef struct ReputationStats {
    // Network health
    float average_reputation;       // Average network reputation
    float reputation_variance;      // Reputation variance
    size_t high_reputation_peers;   // Peers with high reputation
    size_t low_reputation_peers;    // Peers with low reputation
    
    // Event statistics
    size_t total_events;           // Total events recorded
    size_t verified_events;        // Verified events
    size_t fraud_attempts;         // Detected fraud attempts
    
    // Trust network
    size_t trust_relationships;    // Total trust relationships
    float average_trust_level;     // Average trust level
    float network_connectivity;    // How connected the network is
    
    // Package quality
    float average_package_quality; // Average package quality
    size_t packages_with_audits;   // Packages with security audits
    size_t vulnerability_reports;  // Total vulnerability reports
    
    // System performance
    float reputation_accuracy;     // How accurate predictions are
    float false_positive_rate;     // False positive rate for fraud
    float false_negative_rate;     // False negative rate for fraud
    
    time_t stats_generated_at;
} ReputationStats;

ReputationStats* reputation_system_get_stats(ReputationSystem* system);
void reputation_stats_free(ReputationStats* stats);

// Configuration management
typedef struct ReputationConfig {
    // Thresholds
    float min_reputation_threshold;
    float trust_decay_rate;
    int reputation_window_days;
    
    // Weights
    float reliability_weight;
    float security_weight;
    float performance_weight;
    float community_weight;
    
    // Verification
    bool require_event_verification;
    int min_verifiers;
    float verification_threshold;
    
    // Anti-gaming
    bool enable_sybil_detection;
    bool enable_collusion_detection;
    float max_self_boost;
} ReputationConfig;

ReputationConfig* reputation_config_create_default(void);
ReputationConfig* reputation_config_load(const char* config_file);
bool reputation_config_save(const ReputationConfig* config, const char* config_file);
void reputation_config_free(ReputationConfig* config);

#endif // REPUTATION_SYSTEM_H