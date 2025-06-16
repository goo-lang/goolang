#ifndef AI_CACHE_H
#define AI_CACHE_H

#include "hybrid_registry.h"
#include "p2p_discovery.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Forward declarations
typedef struct AICacheManager AICacheManager;
typedef struct CacheEntry CacheEntry;
typedef struct CachePrediction CachePrediction;
typedef struct MLModel MLModel;
typedef struct CachePattern CachePattern;
typedef struct SemanticIndex SemanticIndex;

// Cache entry types
typedef enum {
    CACHE_PACKAGE_BINARY,       // Compiled package binary
    CACHE_PACKAGE_SOURCE,       // Source code
    CACHE_METADATA,             // Package metadata
    CACHE_DEPENDENCY_GRAPH,     // Dependency resolution data
    CACHE_COMPILATION_ARTIFACT, // Intermediate compilation results
    CACHE_SEMANTIC_INDEX        // Semantic search index
} CacheEntryType;

// Cache access patterns
typedef enum {
    ACCESS_SEQUENTIAL,          // Sequential access pattern
    ACCESS_RANDOM,              // Random access pattern
    ACCESS_TEMPORAL,            // Time-based access pattern
    ACCESS_DEPENDENCY_DRIVEN,   // Dependency-based access
    ACCESS_PROJECT_SCOPED       // Project-specific access
} AccessPattern;

// Individual cache entry
typedef struct CacheEntry {
    char* key;                  // Unique cache key
    char* content_hash;         // Content hash (SHA256)
    CacheEntryType type;        // Entry type
    
    // Content information
    void* data;                 // Cached data
    size_t size;                // Data size in bytes
    char* file_path;            // Path to cached file (if applicable)
    
    // Metadata
    time_t created_at;          // Cache creation time
    time_t last_accessed;       // Last access time
    time_t expires_at;          // Expiration time (0 = never)
    int access_count;           // Number of accesses
    
    // Dependency information
    char** dependencies;        // What this entry depends on
    size_t dependency_count;
    char** dependents;          // What depends on this entry
    size_t dependent_count;
    
    // AI metadata
    float importance_score;     // AI-computed importance (0.0-1.0)
    float prediction_confidence; // Confidence in predictions
    AccessPattern access_pattern; // Detected access pattern
    
    // Performance metrics
    float avg_access_time_ms;   // Average access time
    size_t compression_ratio;   // Compression efficiency
    bool is_frequently_used;    // Frequently accessed flag
    
    struct CacheEntry* next;    // Linked list
} CacheEntry;

// Cache prediction for prefetching
typedef struct CachePrediction {
    char* predicted_key;        // Predicted cache key
    float probability;          // Prediction probability (0.0-1.0)
    time_t predicted_time;      // When it will be needed
    
    // Prediction metadata
    char* reason;               // Why this was predicted
    char** related_entries;     // Related cache entries
    size_t related_count;
    
    // Prefetch strategy
    int priority;               // Prefetch priority (1-10)
    bool should_prefetch;       // Should this be prefetched
    size_t estimated_size;      // Estimated size for prefetch
    
    struct CachePrediction* next;
} CachePrediction;

// Machine learning model for cache optimization
typedef struct MLModel {
    // Model type
    enum {
        ML_LINEAR_REGRESSION,   // Linear regression model
        ML_DECISION_TREE,       // Decision tree
        ML_NEURAL_NETWORK,      // Simple neural network
        ML_ENSEMBLE,            // Ensemble of models
        ML_REINFORCEMENT        // Reinforcement learning
    } type;
    
    // Model parameters
    float* weights;             // Model weights
    size_t weight_count;
    float* biases;              // Model biases
    size_t bias_count;
    
    // Training data
    float** training_features;  // Feature vectors
    float* training_labels;     // Training labels
    size_t training_size;
    
    // Model performance
    float accuracy;             // Model accuracy
    float precision;            // Prediction precision
    float recall;               // Prediction recall
    time_t last_trained;        // Last training time
    
    // Prediction functions
    float (*predict_access_probability)(struct MLModel* model, CacheEntry* entry);
    float (*predict_lifetime)(struct MLModel* model, CacheEntry* entry);
    float (*predict_importance)(struct MLModel* model, CacheEntry* entry);
} MLModel;

// Cache usage patterns detected by AI
typedef struct CachePattern {
    char* pattern_id;           // Unique pattern identifier
    char* description;          // Human-readable description
    
    // Pattern characteristics
    AccessPattern type;         // Pattern type
    float confidence;           // Pattern confidence (0.0-1.0)
    time_t detected_at;         // When pattern was detected
    
    // Temporal information
    int time_window_hours;      // Time window for pattern
    char** peak_hours;          // Peak usage hours
    size_t peak_hour_count;
    
    // Access sequences
    char** access_sequence;     // Common access sequences
    size_t sequence_length;
    float sequence_probability; // Probability of sequence
    
    // Project correlation
    char** correlated_projects; // Projects showing this pattern
    size_t project_count;
    
    struct CachePattern* next;
} CachePattern;

// Semantic indexing for intelligent search
typedef struct SemanticIndex {
    // Vector embeddings
    float** embeddings;         // Package semantic embeddings
    size_t embedding_dim;       // Embedding dimension
    size_t package_count;       // Number of indexed packages
    
    // Index metadata
    char** package_names;       // Package names
    char** descriptions;        // Package descriptions
    char** keywords;            // Extracted keywords
    
    // Similarity computation
    float (*compute_similarity)(struct SemanticIndex* index, 
                               const char* query, 
                               const char* package);
    char** (*find_similar)(struct SemanticIndex* index,
                          const char* query,
                          int max_results,
                          float min_similarity);
    
    // Clustering information
    int* cluster_assignments;   // Cluster assignments
    size_t cluster_count;       // Number of clusters
    float** cluster_centers;    // Cluster centroids
} SemanticIndex;

// AI-powered cache manager
typedef struct AICacheManager {
    // Cache storage
    CacheEntry** entries;       // Cache entries
    size_t entry_count;
    size_t entry_capacity;
    
    // Cache configuration
    size_t max_cache_size;      // Maximum cache size (bytes)
    size_t current_cache_size;  // Current cache size
    char* cache_directory;      // Cache storage directory
    
    // AI components
    MLModel** models;           // ML models for optimization
    size_t model_count;
    CachePattern* patterns;     // Detected usage patterns
    SemanticIndex* semantic_index; // Semantic search index
    
    // Prediction system
    CachePrediction* predictions; // Current predictions
    bool enable_prefetching;    // Enable predictive prefetching
    int prefetch_lookahead_hours; // How far ahead to predict
    
    // Performance optimization
    bool enable_compression;    // Enable data compression
    bool enable_deduplication;  // Enable content deduplication
    bool enable_clustering;     // Enable cache clustering
    
    // Learning system
    bool enable_online_learning; // Enable continuous learning
    float learning_rate;        // Learning rate for model updates
    int retrain_interval_hours; // How often to retrain models
    
    // Statistics
    size_t cache_hits;          // Cache hit count
    size_t cache_misses;        // Cache miss count
    size_t prefetch_hits;       // Successful prefetches
    size_t prefetch_misses;     // Failed prefetches
    float avg_access_time_ms;   // Average access time
    
    // Background processing
    pthread_t optimization_thread; // Background optimization thread
    pthread_t prefetch_thread;  // Prefetching thread
    bool threads_active;        // Are threads running
    
    // Integration components
    HybridRegistry* registry;   // Package registry
    P2PDiscovery* p2p_discovery; // P2P network
} AICacheManager;

// Function declarations

// AI Cache Manager lifecycle
AICacheManager* ai_cache_create(void);
void ai_cache_free(AICacheManager* cache);
bool ai_cache_initialize(AICacheManager* cache);
bool ai_cache_start_background_threads(AICacheManager* cache);
bool ai_cache_stop_background_threads(AICacheManager* cache);

// Cache operations
CacheEntry* ai_cache_get(AICacheManager* cache, const char* key);
bool ai_cache_put(AICacheManager* cache, const char* key, const void* data, 
                  size_t size, CacheEntryType type);
bool ai_cache_remove(AICacheManager* cache, const char* key);
bool ai_cache_clear(AICacheManager* cache);
bool ai_cache_invalidate_pattern(AICacheManager* cache, const char* pattern);

// AI-powered cache optimization
bool ai_cache_optimize(AICacheManager* cache);
bool ai_cache_predict_access(AICacheManager* cache);
bool ai_cache_prefetch_likely(AICacheManager* cache);
float ai_cache_compute_importance(AICacheManager* cache, CacheEntry* entry);

// Machine learning operations
MLModel* ai_cache_create_model(int model_type);
bool ai_cache_train_model(AICacheManager* cache, MLModel* model);
bool ai_cache_update_model_online(AICacheManager* cache, MLModel* model,
                                 const char* key, bool was_hit);
CachePrediction** ai_cache_predict_future_access(AICacheManager* cache,
                                                int hours_ahead,
                                                size_t* prediction_count);

// Pattern recognition
bool ai_cache_detect_patterns(AICacheManager* cache);
CachePattern* ai_cache_find_pattern(AICacheManager* cache, const char* key);
bool ai_cache_update_patterns(AICacheManager* cache, const char* key,
                             time_t access_time);

// Semantic indexing
bool ai_cache_build_semantic_index(AICacheManager* cache);
char** ai_cache_semantic_search(AICacheManager* cache, const char* query,
                               size_t* result_count);
float ai_cache_compute_semantic_similarity(AICacheManager* cache,
                                          const char* key1, const char* key2);

// Intelligent eviction
bool ai_cache_smart_evict(AICacheManager* cache, size_t bytes_needed);
CacheEntry** ai_cache_rank_for_eviction(AICacheManager* cache,
                                       size_t* entry_count);
bool ai_cache_should_evict(AICacheManager* cache, CacheEntry* entry);

// Compression and deduplication
bool ai_cache_compress_entry(AICacheManager* cache, CacheEntry* entry);
bool ai_cache_decompress_entry(AICacheManager* cache, CacheEntry* entry);
bool ai_cache_deduplicate(AICacheManager* cache);
char* ai_cache_find_duplicate(AICacheManager* cache, const char* content_hash);

// Cache warming and prefetching
bool ai_cache_warm_from_project(AICacheManager* cache, const char* project_path);
bool ai_cache_prefetch_dependencies(AICacheManager* cache, const char* package_name);
bool ai_cache_prefetch_related(AICacheManager* cache, const char* key);

// Performance monitoring
typedef struct CacheStats {
    // Hit/miss statistics
    size_t total_requests;
    size_t cache_hits;
    size_t cache_misses;
    float hit_rate;
    
    // Prefetch statistics
    size_t prefetch_requests;
    size_t prefetch_hits;
    size_t prefetch_misses;
    float prefetch_accuracy;
    
    // Performance metrics
    float avg_access_time_ms;
    float avg_compression_ratio;
    size_t deduplication_savings;
    
    // AI model performance
    float ml_prediction_accuracy;
    float pattern_detection_confidence;
    float semantic_search_quality;
    
    // Cache utilization
    size_t current_size;
    size_t max_size;
    float utilization_rate;
    size_t entry_count;
    
    // Time-based metrics
    time_t stats_period_start;
    time_t stats_period_end;
} CacheStats;

CacheStats* ai_cache_get_stats(AICacheManager* cache);
void ai_cache_stats_free(CacheStats* stats);
bool ai_cache_reset_stats(AICacheManager* cache);

// Cache entry operations
CacheEntry* cache_entry_create(const char* key, const void* data, size_t size,
                              CacheEntryType type);
void cache_entry_free(CacheEntry* entry);
bool cache_entry_update_access(CacheEntry* entry);
bool cache_entry_add_dependency(CacheEntry* entry, const char* dependency);
float cache_entry_compute_score(const CacheEntry* entry);

// ML model operations
void ml_model_free(MLModel* model);
bool ml_model_train(MLModel* model, float** features, float* labels, size_t count);
float ml_model_predict(MLModel* model, float* features);
bool ml_model_save(const MLModel* model, const char* filename);
MLModel* ml_model_load(const char* filename);

// Pattern operations
CachePattern* cache_pattern_create(const char* pattern_id, AccessPattern type);
void cache_pattern_free(CachePattern* pattern);
bool cache_pattern_matches(const CachePattern* pattern, const char* key,
                          time_t access_time);

// Semantic index operations
SemanticIndex* semantic_index_create(size_t embedding_dim);
void semantic_index_free(SemanticIndex* index);
bool semantic_index_add_package(SemanticIndex* index, const char* name,
                               const char* description);
bool semantic_index_build_embeddings(SemanticIndex* index);

// Configuration management
typedef struct AICacheConfig {
    // Cache settings
    size_t max_cache_size;
    char* cache_directory;
    bool enable_compression;
    bool enable_deduplication;
    
    // AI settings
    bool enable_prefetching;
    bool enable_online_learning;
    float learning_rate;
    int retrain_interval_hours;
    int prefetch_lookahead_hours;
    
    // Performance settings
    bool enable_clustering;
    bool enable_semantic_search;
    int background_thread_count;
} AICacheConfig;

AICacheConfig* ai_cache_config_create_default(void);
AICacheConfig* ai_cache_config_load(const char* config_file);
bool ai_cache_config_save(const AICacheConfig* config, const char* config_file);
void ai_cache_config_free(AICacheConfig* config);

// Integration with package system
bool ai_cache_integrate_registry(AICacheManager* cache, HybridRegistry* registry);
bool ai_cache_integrate_p2p(AICacheManager* cache, P2PDiscovery* p2p);
bool ai_cache_learn_from_build(AICacheManager* cache, const char* project_path);

#endif // AI_CACHE_H