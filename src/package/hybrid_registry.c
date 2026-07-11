#include "package/hybrid_registry.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <json-c/json.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <errno.h>

// HTTP response structure
typedef struct {
    char* data;
    size_t size;
} HTTPResponse;

// Thread pool for parallel downloads
typedef struct {
    pthread_t* threads;
    int thread_count;
    bool active;
} ThreadPool;

static ThreadPool* g_download_pool = NULL;
static pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

// HTTP callback
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

// Create cache directory if it doesn't exist
static bool ensure_cache_directory(const char* cache_dir) {
    struct stat st = {0};
    if (stat(cache_dir, &st) == -1) {
        if (mkdir(cache_dir, 0755) != 0) {
            return false;
        }
    }
    return true;
}

HybridRegistry* hybrid_registry_create(void) {
    HybridRegistry* registry = xcalloc(1, sizeof(HybridRegistry));
    if (!registry) return NULL;
    
    // Create sub-components
    registry->ipfs_client = ipfs_client_create();
    registry->ipns_manager = ipns_manager_create(registry->ipfs_client);
    registry->gateway_intelligence = gateway_intelligence_create(registry->ipfs_client);
    registry->crypto_verifier = crypto_verifier_create();
    registry->p2p_discovery = p2p_discovery_create(registry->ipfs_client, registry->crypto_verifier);
    
    if (!registry->ipfs_client || !registry->ipns_manager || 
        !registry->gateway_intelligence || !registry->crypto_verifier ||
        !registry->p2p_discovery) {
        hybrid_registry_free(registry);
        return NULL;
    }
    
    // Default configuration
    registry->default_strategy = hybrid_registry_create_strategy();
    registry->auto_discover_registries = true;
    registry->enable_blockchain_verification = false;
    registry->enable_predictive_caching = true;
    registry->enable_bandwidth_sharing = true;
    registry->max_concurrent_downloads = 5;
    registry->require_https = true;
    registry->verify_all_downloads = true;
    registry->min_trust_score = 0.5f;
    
    // Cache configuration
    registry->cache_directory = strdup("/tmp/goo-package-cache");
    registry->cache_capacity = 100;
    registry->cached_metadata = calloc(registry->cache_capacity, sizeof(PackageMetadata*));
    
    return registry;
}

void hybrid_registry_free(HybridRegistry* registry) {
    if (!registry) return;
    
    // Free backends
    RegistryBackend* backend = registry->backends;
    while (backend) {
        RegistryBackend* next = backend->next;
        registry_backend_free(backend);
        backend = next;
    }
    
    // Free cached metadata
    for (size_t i = 0; i < registry->cache_size; i++) {
        package_metadata_free(registry->cached_metadata[i]);
    }
    free(registry->cached_metadata);
    
    // Free sub-components
    ipfs_client_free(registry->ipfs_client);
    ipns_manager_free(registry->ipns_manager);
    gateway_intelligence_free(registry->gateway_intelligence);
    crypto_verifier_free(registry->crypto_verifier);
    p2p_discovery_free(registry->p2p_discovery);
    
    hybrid_registry_free_strategy(registry->default_strategy);
    free(registry->cache_directory);
    free(registry);
}

bool hybrid_registry_initialize(HybridRegistry* registry) {
    if (!registry) return false;
    
    // Initialize sub-components
    if (!ipfs_client_connect(registry->ipfs_client)) {
        // IPFS not available, continue with traditional registries
    }
    
    ipns_manager_initialize(registry->ipns_manager);
    gateway_intelligence_initialize(registry->gateway_intelligence);
    crypto_verifier_initialize(registry->crypto_verifier);
    p2p_discovery_initialize(registry->p2p_discovery);
    
    // Create cache directory
    ensure_cache_directory(registry->cache_directory);
    
    // Add default backends
    
    // 1. Traditional NPM-style registry
    RegistryBackend* npm_backend = registry_backend_create(
        BACKEND_TRADITIONAL, "npm", "https://registry.npmjs.org"
    );
    npm_backend->supports_search = true;
    npm_backend->supports_metadata = true;
    npm_backend->supports_versioning = true;
    npm_backend->priority = 10;
    hybrid_registry_add_backend(registry, npm_backend);
    
    // 2. IPFS backend
    if (registry->ipfs_client->is_connected) {
        RegistryBackend* ipfs_backend = registry_backend_create(
            BACKEND_IPFS, "ipfs", "ipfs://localhost:5001"
        );
        ipfs_backend->supports_binary = true;
        ipfs_backend->supports_signing = true;
        ipfs_backend->priority = 5;
        hybrid_registry_add_backend(registry, ipfs_backend);
    }
    
    // 3. Local cache backend
    RegistryBackend* cache_backend = registry_backend_create(
        BACKEND_LOCAL_CACHE, "cache", registry->cache_directory
    );
    cache_backend->supports_binary = true;
    cache_backend->reliability_score = 1.0f;
    cache_backend->avg_latency_ms = 1.0f;
    cache_backend->priority = 1;
    hybrid_registry_add_backend(registry, cache_backend);
    
    // 4. P2P discovery backend
    RegistryBackend* p2p_backend = registry_backend_create(
        BACKEND_P2P, "p2p", "p2p://goo-packages"
    );
    p2p_backend->supports_search = true;
    p2p_backend->supports_binary = true;
    p2p_backend->priority = 7;
    hybrid_registry_add_backend(registry, p2p_backend);
    
    // Start P2P discovery
    p2p_discovery_start(registry->p2p_discovery);
    
    // Initialize download thread pool
    g_download_pool = xcalloc(1, sizeof(ThreadPool));
    g_download_pool->thread_count = registry->max_concurrent_downloads;
    g_download_pool->threads = calloc(g_download_pool->thread_count, sizeof(pthread_t));
    g_download_pool->active = true;
    
    return true;
}

bool hybrid_registry_add_backend(HybridRegistry* registry, RegistryBackend* backend) {
    if (!registry || !backend) return false;
    
    pthread_mutex_lock(&g_registry_mutex);
    
    // Add to beginning of list
    backend->next = registry->backends;
    registry->backends = backend;
    registry->backend_count++;
    
    pthread_mutex_unlock(&g_registry_mutex);
    
    return true;
}

RegistryBackend* hybrid_registry_get_backend(HybridRegistry* registry, const char* name) {
    if (!registry || !name) return NULL;
    
    RegistryBackend* backend = registry->backends;
    while (backend) {
        if (strcmp(backend->name, name) == 0) {
            return backend;
        }
        backend = backend->next;
    }
    
    return NULL;
}

PackageMetadata* hybrid_registry_resolve_package(HybridRegistry* registry, 
                                               const char* package_name,
                                               const char* version_spec) {
    if (!registry || !package_name || !version_spec) return NULL;
    
    // Check cache first
    for (size_t i = 0; i < registry->cache_size; i++) {
        PackageMetadata* cached = registry->cached_metadata[i];
        if (cached && strcmp(cached->name, package_name) == 0 &&
            strcmp(cached->version, version_spec) == 0) {
            pthread_mutex_lock(&g_registry_mutex);
            registry->cache_hits++;
            pthread_mutex_unlock(&g_registry_mutex);
            return cached;
        }
    }
    
    pthread_mutex_lock(&g_registry_mutex);
    registry->cache_misses++;
    pthread_mutex_unlock(&g_registry_mutex);
    
    // Try backends according to strategy
    PackageMetadata* metadata = NULL;
    
    if (registry->default_strategy->type == STRATEGY_PARALLEL) {
        // Try all backends in parallel
        // Simplified: try sequentially for now
        RegistryBackend* backend = registry->backends;
        while (backend && !metadata) {
            metadata = hybrid_registry_resolve_from_backend(
                registry, backend, package_name, version_spec
            );
            backend = backend->next;
        }
    } else {
        // Sort backends by priority
        RegistryBackend** sorted_backends = malloc(
            registry->backend_count * sizeof(RegistryBackend*)
        );
        size_t count = 0;
        
        RegistryBackend* backend = registry->backends;
        while (backend) {
            sorted_backends[count++] = backend;
            backend = backend->next;
        }
        
        // Simple bubble sort by priority
        for (size_t i = 0; i < count - 1; i++) {
            for (size_t j = i + 1; j < count; j++) {
                if (sorted_backends[i]->priority > sorted_backends[j]->priority) {
                    RegistryBackend* temp = sorted_backends[i];
                    sorted_backends[i] = sorted_backends[j];
                    sorted_backends[j] = temp;
                }
            }
        }
        
        // Try backends in order
        for (size_t i = 0; i < count && !metadata; i++) {
            metadata = hybrid_registry_resolve_from_backend(
                registry, sorted_backends[i], package_name, version_spec
            );
        }
        
        free(sorted_backends);
    }
    
    // Cache the result
    if (metadata) {
        hybrid_registry_cache_metadata(registry, metadata);
        
        pthread_mutex_lock(&g_registry_mutex);
        registry->total_packages_resolved++;
        pthread_mutex_unlock(&g_registry_mutex);
    }
    
    return metadata;
}

// Helper function to resolve from specific backend
static PackageMetadata* hybrid_registry_resolve_from_backend(
    HybridRegistry* registry,
    RegistryBackend* backend,
    const char* package_name,
    const char* version_spec
) {
    if (!backend->supports_metadata) return NULL;
    
    PackageMetadata* metadata = NULL;
    
    switch (backend->type) {
        case BACKEND_TRADITIONAL: {
            // HTTP GET to registry API
            CURL* curl = curl_easy_init();
            if (!curl) return NULL;
            
            char url[512];
            snprintf(url, sizeof(url), "%s/%s/%s", 
                     backend->url, package_name, version_spec);
            
            HTTPResponse response = {0};
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            
            if (backend->auth_token) {
                char auth_header[256];
                snprintf(auth_header, sizeof(auth_header), 
                         "Authorization: Bearer %s", backend->auth_token);
                struct curl_slist* headers = curl_slist_append(NULL, auth_header);
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            }
            
            CURLcode res = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            
            if (res == CURLE_OK && http_code == 200 && response.data) {
                metadata = package_metadata_deserialize(response.data);
            }
            
            free(response.data);
            curl_easy_cleanup(curl);
            break;
        }
        
        case BACKEND_IPFS: {
            // Search via P2P discovery
            P2PSearchQuery* query = p2p_search_query_create(package_name);
            size_t result_count;
            P2PSearchResult** results = p2p_discovery_search_packages(
                registry->p2p_discovery, query, &result_count
            );
            
            if (results && result_count > 0) {
                // Convert P2P result to metadata
                P2PPackageAnnouncement* announcement = results[0]->announcement;
                metadata = package_metadata_create(announcement->package_name, 
                                                 announcement->version);
                metadata->ipfs_cid = ipfs_cid_clone(announcement->cid);
            }
            
            p2p_search_query_free(query);
            free(results);
            break;
        }
        
        case BACKEND_LOCAL_CACHE: {
            // Check local filesystem
            char cache_path[512];
            snprintf(cache_path, sizeof(cache_path), "%s/%s-%s.json",
                     backend->url, package_name, version_spec);
            
            FILE* f = fopen(cache_path, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                
                char* json_data = malloc(size + 1);
                if (json_data) {
                    fread(json_data, 1, size, f);
                    json_data[size] = '\0';
                    metadata = package_metadata_deserialize(json_data);
                    free(json_data);
                }
                fclose(f);
            }
            break;
        }
        
        default:
            break;
    }
    
    // Update backend statistics
    bool success = (metadata != NULL);
    registry_backend_update_stats(backend, success, 0, 0);
    
    return metadata;
}

// Helper to cache metadata
static void hybrid_registry_cache_metadata(HybridRegistry* registry, 
                                         PackageMetadata* metadata) {
    pthread_mutex_lock(&g_registry_mutex);
    
    if (registry->cache_size >= registry->cache_capacity) {
        // Simple FIFO eviction
        package_metadata_free(registry->cached_metadata[0]);
        memmove(registry->cached_metadata, registry->cached_metadata + 1,
                (registry->cache_size - 1) * sizeof(PackageMetadata*));
        registry->cache_size--;
    }
    
    registry->cached_metadata[registry->cache_size++] = metadata;
    
    pthread_mutex_unlock(&g_registry_mutex);
}

DownloadPlan* hybrid_registry_create_download_plan(HybridRegistry* registry,
                                                  PackageMetadata* package) {
    if (!registry || !package) return NULL;
    
    DownloadPlan* plan = xcalloc(1, sizeof(DownloadPlan));
    if (!plan) return NULL;
    
    plan->package = package;
    plan->total_size = 0; // Would be set from metadata
    
    // Collect download sources
    plan->source_count = 0;
    plan->sources = calloc(registry->backend_count, sizeof(DownloadPlan*));
    
    // Add IPFS source if available
    if (package->ipfs_cid) {
        DownloadPlan* source = xcalloc(1, sizeof(DownloadPlan));
        source->backend_type = BACKEND_IPFS;
        source->source_url = strdup(package->ipfs_cid->hash);
        source->expected_speed = 10.0f; // Estimate
        source->reliability = 0.9f;
        source->is_verified = false;
        plan->sources[plan->source_count++] = source;
    }
    
    // Add traditional URL if available
    if (package->traditional_url) {
        DownloadPlan* source = xcalloc(1, sizeof(DownloadPlan));
        source->backend_type = BACKEND_TRADITIONAL;
        source->source_url = strdup(package->traditional_url);
        source->expected_speed = 5.0f; // Estimate
        source->reliability = 0.95f;
        source->is_verified = false;
        plan->sources[plan->source_count++] = source;
    }
    
    // Add P2P sources
    P2PSearchQuery* query = p2p_search_query_create(package->name);
    size_t result_count;
    P2PSearchResult** results = p2p_discovery_search_packages(
        registry->p2p_discovery, query, &result_count
    );
    
    if (results) {
        for (size_t i = 0; i < result_count && plan->source_count < 5; i++) {
            if (results[i]->announcement->seeder_count > 0) {
                DownloadPlan* source = xcalloc(1, sizeof(DownloadPlan));
                source->backend_type = BACKEND_P2P;
                source->source_url = strdup(results[i]->announcement->cid->hash);
                source->expected_speed = 8.0f; // Estimate
                source->reliability = results[i]->announcement->availability_score;
                source->is_verified = results[i]->announcement->is_verified;
                plan->sources[plan->source_count++] = source;
            }
        }
    }
    
    p2p_search_query_free(query);
    free(results);
    
    // Calculate estimated download time
    if (plan->source_count > 0 && plan->sources[0]->expected_speed > 0) {
        plan->estimated_time_seconds = plan->total_size / 
                                     (plan->sources[0]->expected_speed * 1024 * 1024);
    }
    
    // Set download strategy
    plan->use_parallel_chunks = (plan->total_size > 10 * 1024 * 1024); // >10MB
    plan->chunk_size = 1024 * 1024; // 1MB chunks
    plan->verify_while_downloading = registry->verify_all_downloads;
    
    return plan;
}

bool hybrid_registry_publish_package(HybridRegistry* registry,
                                   const char* package_path,
                                   PackageMetadata* metadata) {
    if (!registry || !package_path || !metadata) return false;
    
    bool success = false;
    
    // 1. Publish to IPFS
    if (registry->ipfs_client->is_connected) {
        success = hybrid_registry_publish_to_ipfs(registry, package_path, metadata);
    }
    
    // 2. Announce via P2P
    if (success && metadata->ipfs_cid) {
        p2p_discovery_announce_package(registry->p2p_discovery,
                                     metadata->name,
                                     metadata->version,
                                     metadata->ipfs_cid);
    }
    
    // 3. Update traditional registry (if configured)
    RegistryBackend* traditional = hybrid_registry_get_backend(registry, "npm");
    if (traditional) {
        // Would implement traditional registry publishing
    }
    
    return success;
}

bool hybrid_registry_publish_to_ipfs(HybridRegistry* registry,
                                   const char* package_path,
                                   PackageMetadata* metadata) {
    if (!registry || !package_path || !metadata) return false;
    
    // Add file to IPFS
    IpfsCid* cid = ipfs_client_add_file(registry->ipfs_client, package_path);
    if (!cid) return false;
    
    // Update metadata
    if (metadata->ipfs_cid) {
        ipfs_cid_free(metadata->ipfs_cid);
    }
    metadata->ipfs_cid = cid;
    
    // Pin the content
    ipfs_client_pin_add(registry->ipfs_client, cid);
    
    // Update or create IPNS record
    if (metadata->ipns_name) {
        ipns_manager_update_record(registry->ipns_manager, 
                                 metadata->ipns_name, cid);
    } else {
        char* ipns_name = ipns_manager_publish(registry->ipns_manager, 
                                              cid, metadata->name);
        if (ipns_name) {
            metadata->ipns_name = ipns_name;
        }
    }
    
    return true;
}

// Backend operations
RegistryBackend* registry_backend_create(BackendType type, const char* name, 
                                       const char* url) {
    if (!name || !url) return NULL;
    
    RegistryBackend* backend = xcalloc(1, sizeof(RegistryBackend));
    if (!backend) return NULL;
    
    backend->type = type;
    backend->name = strdup(name);
    backend->url = strdup(url);
    
    if (!backend->name || !backend->url) {
        registry_backend_free(backend);
        return NULL;
    }
    
    // Default capabilities based on type
    switch (type) {
        case BACKEND_TRADITIONAL:
            backend->supports_search = true;
            backend->supports_metadata = true;
            backend->supports_versioning = true;
            break;
        case BACKEND_IPFS:
        case BACKEND_P2P:
            backend->supports_binary = true;
            backend->supports_signing = true;
            break;
        case BACKEND_LOCAL_CACHE:
            backend->supports_binary = true;
            backend->supports_metadata = true;
            break;
        default:
            break;
    }
    
    backend->reliability_score = 0.8f;
    backend->priority = 10;
    backend->last_used = time(NULL);
    
    return backend;
}

void registry_backend_free(RegistryBackend* backend) {
    if (!backend) return;
    
    free(backend->name);
    free(backend->url);
    free(backend->api_key);
    free(backend->auth_token);
    free(backend);
}

bool registry_backend_update_stats(RegistryBackend* backend,
                                 bool success,
                                 float latency_ms,
                                 size_t bytes_transferred) {
    if (!backend) return false;
    
    backend->requests_sent++;
    if (success) {
        backend->requests_succeeded++;
    }
    
    // Update average latency (exponential moving average)
    if (latency_ms > 0) {
        if (backend->avg_latency_ms == 0) {
            backend->avg_latency_ms = latency_ms;
        } else {
            backend->avg_latency_ms = 0.9f * backend->avg_latency_ms + 0.1f * latency_ms;
        }
    }
    
    backend->bytes_downloaded += bytes_transferred;
    backend->last_used = time(NULL);
    
    // Update reliability score
    if (backend->requests_sent > 10) {
        backend->reliability_score = (float)backend->requests_succeeded / 
                                   (float)backend->requests_sent;
    }
    
    return true;
}

// Metadata operations
PackageMetadata* package_metadata_create(const char* name, const char* version) {
    if (!name || !version) return NULL;
    
    PackageMetadata* metadata = xcalloc(1, sizeof(PackageMetadata));
    if (!metadata) return NULL;
    
    metadata->name = strdup(name);
    metadata->version = strdup(version);
    
    if (!metadata->name || !metadata->version) {
        package_metadata_free(metadata);
        return NULL;
    }
    
    metadata->published_at = time(NULL);
    metadata->updated_at = time(NULL);
    
    return metadata;
}

void package_metadata_free(PackageMetadata* metadata) {
    if (!metadata) return;
    
    free(metadata->name);
    free(metadata->version);
    free(metadata->description);
    free(metadata->license);
    
    for (size_t i = 0; i < metadata->author_count; i++) {
        free(metadata->authors[i]);
    }
    free(metadata->authors);
    
    ipfs_cid_free(metadata->ipfs_cid);
    free(metadata->ipns_name);
    free(metadata->traditional_url);
    free(metadata->checksum_sha256);
    
    for (size_t i = 0; i < metadata->dependency_count; i++) {
        free(metadata->dependencies[i]);
    }
    free(metadata->dependencies);
    
    for (size_t i = 0; i < metadata->dev_dependency_count; i++) {
        free(metadata->dev_dependencies[i]);
    }
    free(metadata->dev_dependencies);
    
    free(metadata->vulnerability_report);
    free(metadata);
}

char* package_metadata_serialize(const PackageMetadata* metadata) {
    if (!metadata) return NULL;
    
    json_object* root = json_object_new_object();
    
    json_object_object_add(root, "name", json_object_new_string(metadata->name));
    json_object_object_add(root, "version", json_object_new_string(metadata->version));
    
    if (metadata->description) {
        json_object_object_add(root, "description", 
                             json_object_new_string(metadata->description));
    }
    
    if (metadata->ipfs_cid) {
        json_object_object_add(root, "ipfs_cid", 
                             json_object_new_string(metadata->ipfs_cid->hash));
    }
    
    if (metadata->ipns_name) {
        json_object_object_add(root, "ipns_name", 
                             json_object_new_string(metadata->ipns_name));
    }
    
    // Add dependencies
    if (metadata->dependency_count > 0) {
        json_object* deps = json_object_new_array();
        for (size_t i = 0; i < metadata->dependency_count; i++) {
            json_object_array_add(deps, 
                                json_object_new_string(metadata->dependencies[i]));
        }
        json_object_object_add(root, "dependencies", deps);
    }
    
    const char* json_str = json_object_to_json_string_ext(root, 
                                                         JSON_C_TO_STRING_PRETTY);
    char* result = strdup(json_str);
    json_object_put(root);
    
    return result;
}

PackageMetadata* package_metadata_deserialize(const char* json_data) {
    if (!json_data) return NULL;
    
    json_object* root = json_tokener_parse(json_data);
    if (!root) return NULL;
    
    json_object* name_obj;
    json_object* version_obj;
    
    if (!json_object_object_get_ex(root, "name", &name_obj) ||
        !json_object_object_get_ex(root, "version", &version_obj)) {
        json_object_put(root);
        return NULL;
    }
    
    PackageMetadata* metadata = package_metadata_create(
        json_object_get_string(name_obj),
        json_object_get_string(version_obj)
    );
    
    if (!metadata) {
        json_object_put(root);
        return NULL;
    }
    
    // Parse optional fields
    json_object* desc_obj;
    if (json_object_object_get_ex(root, "description", &desc_obj)) {
        metadata->description = strdup(json_object_get_string(desc_obj));
    }
    
    json_object* cid_obj;
    if (json_object_object_get_ex(root, "ipfs_cid", &cid_obj)) {
        metadata->ipfs_cid = ipfs_cid_from_string(json_object_get_string(cid_obj));
    }
    
    json_object* ipns_obj;
    if (json_object_object_get_ex(root, "ipns_name", &ipns_obj)) {
        metadata->ipns_name = strdup(json_object_get_string(ipns_obj));
    }
    
    // Parse dependencies
    json_object* deps_obj;
    if (json_object_object_get_ex(root, "dependencies", &deps_obj) &&
        json_object_is_type(deps_obj, json_type_array)) {
        size_t dep_count = json_object_array_length(deps_obj);
        metadata->dependencies = calloc(dep_count, sizeof(char*));
        metadata->dependency_count = dep_count;
        
        for (size_t i = 0; i < dep_count; i++) {
            json_object* dep = json_object_array_get_idx(deps_obj, i);
            metadata->dependencies[i] = strdup(json_object_get_string(dep));
        }
    }
    
    json_object_put(root);
    return metadata;
}

// Strategy operations
ResolutionStrategy* hybrid_registry_create_strategy(void) {
    ResolutionStrategy* strategy = xcalloc(1, sizeof(ResolutionStrategy));
    if (!strategy) return NULL;
    
    // Default strategy
    strategy->type = STRATEGY_HYBRID_SMART;
    strategy->max_parallel_requests = 3;
    strategy->timeout_per_backend = 30;
    strategy->prefer_ipfs = true;
    strategy->prefer_p2p = true;
    strategy->require_signatures = false;
    strategy->enable_fallback = true;
    strategy->max_fallback_attempts = 3;
    
    return strategy;
}

void hybrid_registry_free_strategy(ResolutionStrategy* strategy) {
    if (!strategy) return;
    
    free(strategy->fallback_order);
    free(strategy);
}