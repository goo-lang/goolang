#include "package/ai_cache.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <zlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <json-c/json.h>

// Thread-safe access
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_prefetch_cond = PTHREAD_COND_INITIALIZER;

// Simple hash function for cache keys
static size_t hash_key(const char* key) {
    size_t hash = 5381;
    int c;
    while ((c = *key++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

// Feature extraction for ML models
static float* extract_features(CacheEntry* entry, size_t* feature_count) {
    *feature_count = 8; // Number of features
    float* features = calloc(*feature_count, sizeof(float));
    if (!features) return NULL;
    
    time_t now = time(NULL);
    
    // Feature 0: Age (normalized)
    features[0] = (float)(now - entry->created_at) / (24 * 3600); // Days
    
    // Feature 1: Access frequency (normalized)
    features[1] = (float)entry->access_count / 100.0f;
    
    // Feature 2: Time since last access (normalized)
    features[2] = (float)(now - entry->last_accessed) / (24 * 3600); // Days
    
    // Feature 3: Entry size (normalized to MB)
    features[3] = (float)entry->size / (1024 * 1024);
    
    // Feature 4: Entry type (categorical)
    features[4] = (float)entry->type / 10.0f;
    
    // Feature 5: Dependency count
    features[5] = (float)entry->dependency_count / 10.0f;
    
    // Feature 6: Average access time
    features[6] = entry->avg_access_time_ms / 1000.0f; // Seconds
    
    // Feature 7: Importance score
    features[7] = entry->importance_score;
    
    return features;
}

// Background optimization thread
static void* optimization_thread(void* arg) {
    AICacheManager* cache = (AICacheManager*)arg;
    
    while (cache->threads_active) {
        // Detect patterns
        ai_cache_detect_patterns(cache);
        
        // Update ML models
        for (size_t i = 0; i < cache->model_count; i++) {
            if (cache->models[i]) {
                // Retrain model periodically
                time_t now = time(NULL);
                if (now - cache->models[i]->last_trained > 
                    cache->retrain_interval_hours * 3600) {
                    ai_cache_train_model(cache, cache->models[i]);
                }
            }
        }
        
        // Optimize cache layout
        ai_cache_optimize(cache);
        
        // Sleep for optimization interval
        sleep(300); // 5 minutes
    }
    
    return NULL;
}

// Background prefetching thread
static void* prefetch_thread(void* arg) {
    AICacheManager* cache = (AICacheManager*)arg;
    
    while (cache->threads_active) {
        if (cache->enable_prefetching) {
            // Generate predictions
            size_t prediction_count;
            CachePrediction** predictions = ai_cache_predict_future_access(
                cache, cache->prefetch_lookahead_hours, &prediction_count
            );
            
            // Execute prefetching
            if (predictions) {
                for (size_t i = 0; i < prediction_count; i++) {
                    if (predictions[i]->should_prefetch && 
                        predictions[i]->probability > 0.7f) {
                        // Prefetch the predicted entry
                        ai_cache_prefetch_related(cache, predictions[i]->predicted_key);
                    }
                }
                
                // Free predictions
                for (size_t i = 0; i < prediction_count; i++) {
                    free(predictions[i]->predicted_key);
                    free(predictions[i]->reason);
                    free(predictions[i]);
                }
                free(predictions);
            }
        }
        
        pthread_mutex_lock(&g_cache_mutex);
        pthread_cond_wait(&g_prefetch_cond, &g_cache_mutex);
        pthread_mutex_unlock(&g_cache_mutex);
    }
    
    return NULL;
}

AICacheManager* ai_cache_create(void) {
    AICacheManager* cache = calloc(1, sizeof(AICacheManager));
    if (!cache) return NULL;
    
    // Default configuration
    cache->max_cache_size = 10ULL * 1024 * 1024 * 1024; // 10GB
    cache->cache_directory = strdup("/tmp/goo-ai-cache");
    
    cache->entry_capacity = 1000;
    cache->entries = calloc(cache->entry_capacity, sizeof(CacheEntry*));
    if (!cache->entries) {
        ai_cache_free(cache);
        return NULL;
    }
    
    // AI configuration
    cache->enable_prefetching = true;
    cache->prefetch_lookahead_hours = 24;
    cache->enable_compression = true;
    cache->enable_deduplication = true;
    cache->enable_clustering = true;
    cache->enable_online_learning = true;
    cache->learning_rate = 0.01f;
    cache->retrain_interval_hours = 24;
    
    // Create ML models
    cache->model_count = 3;
    cache->models = calloc(cache->model_count, sizeof(MLModel*));
    if (cache->models) {
        cache->models[0] = ai_cache_create_model(ML_LINEAR_REGRESSION); // Access prediction
        cache->models[1] = ai_cache_create_model(ML_DECISION_TREE);    // Eviction decision
        cache->models[2] = ai_cache_create_model(ML_NEURAL_NETWORK);   // Importance scoring
    }
    
    // Create semantic index
    cache->semantic_index = semantic_index_create(128); // 128-dimensional embeddings
    
    return cache;
}

void ai_cache_free(AICacheManager* cache) {
    if (!cache) return;
    
    ai_cache_stop_background_threads(cache);
    
    // Free cache entries
    for (size_t i = 0; i < cache->entry_count; i++) {
        cache_entry_free(cache->entries[i]);
    }
    free(cache->entries);
    
    // Free ML models
    for (size_t i = 0; i < cache->model_count; i++) {
        ml_model_free(cache->models[i]);
    }
    free(cache->models);
    
    // Free patterns
    CachePattern* pattern = cache->patterns;
    while (pattern) {
        CachePattern* next = pattern->next;
        cache_pattern_free(pattern);
        pattern = next;
    }
    
    // Free predictions
    CachePrediction* prediction = cache->predictions;
    while (prediction) {
        CachePrediction* next = prediction->next;
        free(prediction->predicted_key);
        free(prediction->reason);
        free(prediction);
        prediction = next;
    }
    
    semantic_index_free(cache->semantic_index);
    free(cache->cache_directory);
    free(cache);
}

bool ai_cache_initialize(AICacheManager* cache) {
    if (!cache) return false;
    
    // Create cache directory
    struct stat st = {0};
    if (stat(cache->cache_directory, &st) == -1) {
        if (mkdir(cache->cache_directory, 0755) != 0) {
            return false;
        }
    }
    
    // Load existing cache entries
    ai_cache_load_from_disk(cache);
    
    // Build semantic index from existing entries
    ai_cache_build_semantic_index(cache);
    
    // Start background threads
    return ai_cache_start_background_threads(cache);
}

bool ai_cache_start_background_threads(AICacheManager* cache) {
    if (!cache || cache->threads_active) return false;
    
    cache->threads_active = true;
    
    if (pthread_create(&cache->optimization_thread, NULL, optimization_thread, cache) != 0) {
        cache->threads_active = false;
        return false;
    }
    
    if (pthread_create(&cache->prefetch_thread, NULL, prefetch_thread, cache) != 0) {
        cache->threads_active = false;
        pthread_cancel(cache->optimization_thread);
        return false;
    }
    
    return true;
}

CacheEntry* ai_cache_get(AICacheManager* cache, const char* key) {
    if (!cache || !key) return NULL;
    
    pthread_mutex_lock(&g_cache_mutex);
    
    CacheEntry* entry = NULL;
    
    // Linear search (would use hash table in production)
    for (size_t i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i] && strcmp(cache->entries[i]->key, key) == 0) {
            entry = cache->entries[i];
            break;
        }
    }
    
    if (entry) {
        // Update access statistics
        cache_entry_update_access(entry);
        cache->cache_hits++;
        
        // Update ML models with positive feedback
        if (cache->enable_online_learning) {
            for (size_t i = 0; i < cache->model_count; i++) {
                ai_cache_update_model_online(cache, cache->models[i], key, true);
            }
        }
        
        // Decompress if needed
        if (cache->enable_compression && entry->data == NULL && entry->file_path) {
            ai_cache_decompress_entry(cache, entry);
        }
    } else {
        cache->cache_misses++;
        
        // Update ML models with negative feedback
        if (cache->enable_online_learning) {
            for (size_t i = 0; i < cache->model_count; i++) {
                ai_cache_update_model_online(cache, cache->models[i], key, false);
            }
        }
    }
    
    pthread_mutex_unlock(&g_cache_mutex);
    
    return entry;
}

bool ai_cache_put(AICacheManager* cache, const char* key, const void* data, 
                  size_t size, CacheEntryType type) {
    if (!cache || !key || !data || size == 0) return false;
    
    pthread_mutex_lock(&g_cache_mutex);
    
    // Check if entry already exists
    CacheEntry* existing = NULL;
    for (size_t i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i] && strcmp(cache->entries[i]->key, key) == 0) {
            existing = cache->entries[i];
            break;
        }
    }
    
    if (existing) {
        // Update existing entry
        free(existing->data);
        existing->data = malloc(size);
        if (!existing->data) {
            pthread_mutex_unlock(&g_cache_mutex);
            return false;
        }
        memcpy(existing->data, data, size);
        existing->size = size;
        existing->last_accessed = time(NULL);
    } else {
        // Create new entry
        CacheEntry* entry = cache_entry_create(key, data, size, type);
        if (!entry) {
            pthread_mutex_unlock(&g_cache_mutex);
            return false;
        }
        
        // Check cache capacity
        if (cache->entry_count >= cache->entry_capacity) {
            // Expand capacity
            size_t new_capacity = cache->entry_capacity * 2;
            CacheEntry** new_entries = realloc(cache->entries, 
                                              new_capacity * sizeof(CacheEntry*));
            if (!new_entries) {
                cache_entry_free(entry);
                pthread_mutex_unlock(&g_cache_mutex);
                return false;
            }
            cache->entries = new_entries;
            cache->entry_capacity = new_capacity;
        }
        
        // Check size limits and evict if necessary
        if (cache->current_cache_size + size > cache->max_cache_size) {
            ai_cache_smart_evict(cache, size);
        }
        
        // Add to cache
        cache->entries[cache->entry_count++] = entry;
        cache->current_cache_size += size;
        
        // Compute importance score using AI
        entry->importance_score = ai_cache_compute_importance(cache, entry);
        
        // Compress if enabled
        if (cache->enable_compression && size > 1024) {
            ai_cache_compress_entry(cache, entry);
        }
    }
    
    // Update patterns
    ai_cache_update_patterns(cache, key, time(NULL));
    
    // Check for deduplication
    if (cache->enable_deduplication) {
        // Would implement content-based deduplication
    }
    
    pthread_mutex_unlock(&g_cache_mutex);
    
    // Signal prefetch thread
    pthread_cond_signal(&g_prefetch_cond);
    
    return true;
}

bool ai_cache_smart_evict(AICacheManager* cache, size_t bytes_needed) {
    if (!cache) return false;
    
    size_t bytes_freed = 0;
    
    // Get eviction candidates ranked by AI
    size_t candidate_count;
    CacheEntry** candidates = ai_cache_rank_for_eviction(cache, &candidate_count);
    
    if (!candidates) return false;
    
    // Evict entries until we have enough space
    for (size_t i = 0; i < candidate_count && bytes_freed < bytes_needed; i++) {
        CacheEntry* entry = candidates[i];
        
        if (ai_cache_should_evict(cache, entry)) {
            bytes_freed += entry->size;
            cache->current_cache_size -= entry->size;
            
            // Remove from cache
            for (size_t j = 0; j < cache->entry_count; j++) {
                if (cache->entries[j] == entry) {
                    // Shift remaining entries
                    memmove(&cache->entries[j], &cache->entries[j + 1],
                           (cache->entry_count - j - 1) * sizeof(CacheEntry*));
                    cache->entry_count--;
                    break;
                }
            }
            
            cache_entry_free(entry);
        }
    }
    
    free(candidates);
    return bytes_freed >= bytes_needed;
}

CacheEntry** ai_cache_rank_for_eviction(AICacheManager* cache, size_t* entry_count) {
    if (!cache || !entry_count) return NULL;
    
    *entry_count = cache->entry_count;
    if (*entry_count == 0) return NULL;
    
    CacheEntry** ranked = malloc(cache->entry_count * sizeof(CacheEntry*));
    if (!ranked) return NULL;
    
    // Copy entries
    memcpy(ranked, cache->entries, cache->entry_count * sizeof(CacheEntry*));
    
    // Sort by eviction score (lower score = higher priority for eviction)
    for (size_t i = 0; i < *entry_count - 1; i++) {
        for (size_t j = i + 1; j < *entry_count; j++) {
            float score_i = cache_entry_compute_score(ranked[i]);
            float score_j = cache_entry_compute_score(ranked[j]);
            
            if (score_i > score_j) {
                CacheEntry* temp = ranked[i];
                ranked[i] = ranked[j];
                ranked[j] = temp;
            }
        }
    }
    
    return ranked;
}

float ai_cache_compute_importance(AICacheManager* cache, CacheEntry* entry) {
    if (!cache || !entry) return 0.0f;
    
    float importance = 0.5f; // Base importance
    
    // Use neural network model if available
    if (cache->model_count > 2 && cache->models[2]) {
        size_t feature_count;
        float* features = extract_features(entry, &feature_count);
        if (features) {
            importance = ml_model_predict(cache->models[2], features);
            free(features);
        }
    } else {
        // Fallback heuristics
        time_t now = time(NULL);
        
        // Recent access increases importance
        float recency = 1.0f / (1.0f + (now - entry->last_accessed) / 3600.0f);
        importance += recency * 0.3f;
        
        // Frequent access increases importance
        float frequency = fminf((float)entry->access_count / 10.0f, 1.0f);
        importance += frequency * 0.3f;
        
        // Dependency count increases importance
        float deps = fminf((float)entry->dependency_count / 5.0f, 1.0f);
        importance += deps * 0.2f;
    }
    
    return fminf(fmaxf(importance, 0.0f), 1.0f);
}

bool ai_cache_detect_patterns(AICacheManager* cache) {
    if (!cache) return false;
    
    // Analyze access patterns across time windows
    time_t now = time(NULL);
    time_t start_time = now - 7 * 24 * 3600; // Last 7 days
    
    // Count accesses per hour
    int hourly_counts[24] = {0};
    
    for (size_t i = 0; i < cache->entry_count; i++) {
        CacheEntry* entry = cache->entries[i];
        if (entry->last_accessed > start_time) {
            struct tm* tm_info = localtime(&entry->last_accessed);
            hourly_counts[tm_info->tm_hour]++;
        }
    }
    
    // Detect peak hours
    int max_count = 0;
    for (int i = 0; i < 24; i++) {
        if (hourly_counts[i] > max_count) {
            max_count = hourly_counts[i];
        }
    }
    
    // Create or update temporal pattern
    CachePattern* temporal_pattern = ai_cache_find_pattern(cache, "temporal_daily");
    if (!temporal_pattern) {
        temporal_pattern = cache_pattern_create("temporal_daily", ACCESS_TEMPORAL);
        temporal_pattern->next = cache->patterns;
        cache->patterns = temporal_pattern;
    }
    
    temporal_pattern->confidence = 0.8f; // Would compute actual confidence
    temporal_pattern->detected_at = now;
    
    return true;
}

CachePrediction** ai_cache_predict_future_access(AICacheManager* cache,
                                                int hours_ahead,
                                                size_t* prediction_count) {
    if (!cache || !prediction_count) return NULL;
    
    *prediction_count = 0;
    
    // Use linear regression model for predictions
    if (cache->model_count > 0 && cache->models[0]) {
        MLModel* model = cache->models[0];
        
        // Generate predictions for each cache entry
        CachePrediction** predictions = calloc(cache->entry_count, 
                                             sizeof(CachePrediction*));
        if (!predictions) return NULL;
        
        for (size_t i = 0; i < cache->entry_count; i++) {
            CacheEntry* entry = cache->entries[i];
            
            size_t feature_count;
            float* features = extract_features(entry, &feature_count);
            if (!features) continue;
            
            float probability = ml_model_predict(model, features);
            free(features);
            
            if (probability > 0.5f) {
                CachePrediction* pred = calloc(1, sizeof(CachePrediction));
                if (pred) {
                    pred->predicted_key = strdup(entry->key);
                    pred->probability = probability;
                    pred->predicted_time = time(NULL) + hours_ahead * 3600;
                    pred->priority = (int)(probability * 10);
                    pred->should_prefetch = (probability > 0.7f);
                    pred->reason = strdup("ML model prediction");
                    
                    predictions[*prediction_count] = pred;
                    (*prediction_count)++;
                }
            }
        }
        
        return predictions;
    }
    
    return NULL;
}

// Cache entry operations
CacheEntry* cache_entry_create(const char* key, const void* data, size_t size,
                              CacheEntryType type) {
    if (!key || !data || size == 0) return NULL;
    
    CacheEntry* entry = calloc(1, sizeof(CacheEntry));
    if (!entry) return NULL;
    
    entry->key = strdup(key);
    entry->data = malloc(size);
    if (!entry->key || !entry->data) {
        cache_entry_free(entry);
        return NULL;
    }
    
    memcpy(entry->data, data, size);
    entry->size = size;
    entry->type = type;
    
    entry->created_at = time(NULL);
    entry->last_accessed = time(NULL);
    entry->access_count = 1;
    entry->importance_score = 0.5f;
    entry->access_pattern = ACCESS_SEQUENTIAL;
    
    return entry;
}

void cache_entry_free(CacheEntry* entry) {
    if (!entry) return;
    
    free(entry->key);
    free(entry->content_hash);
    free(entry->data);
    free(entry->file_path);
    
    for (size_t i = 0; i < entry->dependency_count; i++) {
        free(entry->dependencies[i]);
    }
    free(entry->dependencies);
    
    for (size_t i = 0; i < entry->dependent_count; i++) {
        free(entry->dependents[i]);
    }
    free(entry->dependents);
    
    free(entry);
}

bool cache_entry_update_access(CacheEntry* entry) {
    if (!entry) return false;
    
    time_t now = time(NULL);
    entry->access_count++;
    
    // Update average access time (simple moving average)
    if (entry->access_count > 1) {
        float time_diff = (float)(now - entry->last_accessed) * 1000.0f; // ms
        entry->avg_access_time_ms = (entry->avg_access_time_ms + time_diff) / 2.0f;
    }
    
    entry->last_accessed = now;
    
    // Update frequently used flag
    entry->is_frequently_used = (entry->access_count > 10);
    
    return true;
}

float cache_entry_compute_score(const CacheEntry* entry) {
    if (!entry) return 0.0f;
    
    time_t now = time(NULL);
    
    // Base score from importance
    float score = entry->importance_score;
    
    // Adjust for recency
    float days_since_access = (float)(now - entry->last_accessed) / (24 * 3600);
    score *= expf(-days_since_access / 7.0f); // Exponential decay over week
    
    // Adjust for frequency
    score *= logf(1.0f + entry->access_count);
    
    // Adjust for size (smaller is better for eviction)
    score *= 1.0f / (1.0f + entry->size / (1024 * 1024)); // MB
    
    return score;
}

// ML model operations
MLModel* ai_cache_create_model(int model_type) {
    MLModel* model = calloc(1, sizeof(MLModel));
    if (!model) return NULL;
    
    model->type = model_type;
    
    switch (model_type) {
        case ML_LINEAR_REGRESSION:
            model->weight_count = 8; // Number of features
            model->weights = calloc(model->weight_count, sizeof(float));
            model->bias_count = 1;
            model->biases = calloc(model->bias_count, sizeof(float));
            
            // Initialize with small random weights
            for (size_t i = 0; i < model->weight_count; i++) {
                model->weights[i] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
            }
            break;
            
        case ML_DECISION_TREE:
            // Simplified decision tree
            model->weight_count = 16; // Tree parameters
            model->weights = calloc(model->weight_count, sizeof(float));
            break;
            
        case ML_NEURAL_NETWORK:
            // Simple feedforward network
            model->weight_count = 64; // Hidden layer weights
            model->weights = calloc(model->weight_count, sizeof(float));
            model->bias_count = 8;
            model->biases = calloc(model->bias_count, sizeof(float));
            break;
            
        default:
            ml_model_free(model);
            return NULL;
    }
    
    if (!model->weights || (model->bias_count > 0 && !model->biases)) {
        ml_model_free(model);
        return NULL;
    }
    
    model->accuracy = 0.5f;
    model->precision = 0.5f;
    model->recall = 0.5f;
    
    return model;
}

void ml_model_free(MLModel* model) {
    if (!model) return;
    
    free(model->weights);
    free(model->biases);
    
    // Free training data
    if (model->training_features) {
        for (size_t i = 0; i < model->training_size; i++) {
            free(model->training_features[i]);
        }
        free(model->training_features);
    }
    free(model->training_labels);
    
    free(model);
}

float ml_model_predict(MLModel* model, float* features) {
    if (!model || !features) return 0.0f;
    
    switch (model->type) {
        case ML_LINEAR_REGRESSION: {
            float result = model->biases[0];
            for (size_t i = 0; i < model->weight_count; i++) {
                result += model->weights[i] * features[i];
            }
            // Apply sigmoid for probability
            return 1.0f / (1.0f + expf(-result));
        }
        
        case ML_NEURAL_NETWORK: {
            // Simple feedforward prediction
            float hidden[8] = {0};
            
            // Hidden layer
            for (int i = 0; i < 8; i++) {
                hidden[i] = model->biases[i];
                for (size_t j = 0; j < model->weight_count / 8; j++) {
                    hidden[i] += model->weights[i * 8 + j] * features[j];
                }
                hidden[i] = fmaxf(0, hidden[i]); // ReLU activation
            }
            
            // Output layer (simplified)
            float output = 0;
            for (int i = 0; i < 8; i++) {
                output += hidden[i] * 0.125f; // Equal weights
            }
            
            return 1.0f / (1.0f + expf(-output)); // Sigmoid
        }
        
        default:
            return 0.5f;
    }
}

// Statistics
CacheStats* ai_cache_get_stats(AICacheManager* cache) {
    if (!cache) return NULL;
    
    CacheStats* stats = calloc(1, sizeof(CacheStats));
    if (!stats) return NULL;
    
    pthread_mutex_lock(&g_cache_mutex);
    
    stats->total_requests = cache->cache_hits + cache->cache_misses;
    stats->cache_hits = cache->cache_hits;
    stats->cache_misses = cache->cache_misses;
    stats->hit_rate = (stats->total_requests > 0) ? 
                     (float)stats->cache_hits / stats->total_requests : 0.0f;
    
    stats->prefetch_requests = cache->prefetch_hits + cache->prefetch_misses;
    stats->prefetch_hits = cache->prefetch_hits;
    stats->prefetch_misses = cache->prefetch_misses;
    stats->prefetch_accuracy = (stats->prefetch_requests > 0) ?
                              (float)stats->prefetch_hits / stats->prefetch_requests : 0.0f;
    
    stats->avg_access_time_ms = cache->avg_access_time_ms;
    stats->current_size = cache->current_cache_size;
    stats->max_size = cache->max_cache_size;
    stats->utilization_rate = (float)stats->current_size / stats->max_size;
    stats->entry_count = cache->entry_count;
    
    // Calculate AI model performance
    float total_accuracy = 0.0f;
    for (size_t i = 0; i < cache->model_count; i++) {
        if (cache->models[i]) {
            total_accuracy += cache->models[i]->accuracy;
        }
    }
    stats->ml_prediction_accuracy = (cache->model_count > 0) ?
                                   total_accuracy / cache->model_count : 0.0f;
    
    stats->stats_period_start = time(NULL);
    
    pthread_mutex_unlock(&g_cache_mutex);
    
    return stats;
}

void ai_cache_stats_free(CacheStats* stats) {
    free(stats);
}