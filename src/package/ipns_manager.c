#include "package/ipns_manager.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

// Response structure for HTTP requests
typedef struct {
    char* data;
    size_t size;
} HTTPResponse;

// Subscription monitor thread data
typedef struct {
    IpnsManager* manager;
    pthread_t thread;
    bool should_stop;
} SubscriptionMonitor;

static SubscriptionMonitor* g_subscription_monitor = NULL;

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

// Generate base58 encoded key ID from public key
static char* generate_key_id(const char* public_key) {
    if (!public_key) return NULL;
    
    // Simplified key ID generation (in practice would use proper base58 encoding)
    char* key_id = malloc(64);
    if (!key_id) return NULL;
    
    snprintf(key_id, 64, "k51qzi5uqu5d%s", public_key + strlen(public_key) - 32);
    return key_id;
}

// Subscription monitoring thread
static void* subscription_monitor_thread(void* arg) {
    SubscriptionMonitor* monitor = (SubscriptionMonitor*)arg;
    IpnsManager* manager = monitor->manager;
    
    while (!monitor->should_stop) {
        // Check all active subscriptions
        IpnsSubscription* sub = manager->subscriptions;
        while (sub) {
            if (sub->is_active) {
                time_t now = time(NULL);
                if (now - sub->last_check >= sub->check_interval) {
                    ipns_subscription_check_update(sub, manager);
                    sub->last_check = now;
                }
            }
            sub = sub->next;
        }
        
        // Sleep for 1 second before next check
        sleep(1);
    }
    
    return NULL;
}

IpnsManager* ipns_manager_create(IpfsClient* ipfs_client) {
    IpnsManager* manager = calloc(1, sizeof(IpnsManager));
    if (!manager) return NULL;
    
    manager->ipfs_client = ipfs_client;
    
    // Default configuration
    manager->default_ttl = 86400;           // 24 hours
    manager->resolution_timeout = 30;       // 30 seconds
    manager->cache_duration = 3600;         // 1 hour
    manager->auto_republish_enabled = true;
    manager->background_resolution = true;
    manager->max_cache_size = 1000;
    manager->enable_prediction = true;
    manager->batch_operations = false;
    
    return manager;
}

void ipns_manager_free(IpnsManager* manager) {
    if (!manager) return;
    
    // Stop subscription monitoring
    ipns_manager_stop_subscription_monitor(manager);
    
    // Free keys
    IpnsKey* key = manager->keys;
    while (key) {
        IpnsKey* next = key->next;
        ipns_key_free(key);
        key = next;
    }
    
    // Free records
    IpnsRecord* record = manager->records;
    while (record) {
        IpnsRecord* next = record->next;
        ipns_record_free(record);
        record = next;
    }
    
    // Free subscriptions
    IpnsSubscription* sub = manager->subscriptions;
    while (sub) {
        IpnsSubscription* next = sub->next;
        ipns_subscription_free(sub);
        sub = next;
    }
    
    free(manager->keystore_path);
    free(manager);
}

bool ipns_manager_initialize(IpnsManager* manager) {
    if (!manager) return false;
    
    // Initialize OpenSSL
    OpenSSL_add_all_algorithms();
    
    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Create default key if none exists
    if (!manager->keys) {
        IpnsKey* default_key = ipns_manager_create_key(manager, "default");
        if (default_key) {
            ipns_manager_set_default_key(manager, "default");
        }
    }
    
    return true;
}

IpnsKey* ipns_manager_create_key(IpnsManager* manager, const char* key_name) {
    if (!manager || !key_name) return NULL;
    
    IpnsKey* key = ipns_key_create(key_name);
    if (!key) return NULL;
    
    if (!ipns_key_generate_keypair(key)) {
        ipns_key_free(key);
        return NULL;
    }
    
    // Add to manager's key list
    key->next = manager->keys;
    manager->keys = key;
    
    return key;
}

bool ipns_manager_set_default_key(IpnsManager* manager, const char* key_name) {
    if (!manager || !key_name) return false;
    
    IpnsKey* key = ipns_manager_get_key(manager, key_name);
    if (!key) return false;
    
    // Clear previous default
    IpnsKey* current = manager->keys;
    while (current) {
        current->is_default = false;
        current = current->next;
    }
    
    // Set new default
    key->is_default = true;
    manager->default_key = key;
    
    return true;
}

IpnsKey* ipns_manager_get_key(IpnsManager* manager, const char* key_name) {
    if (!manager || !key_name) return NULL;
    
    IpnsKey* key = manager->keys;
    while (key) {
        if (strcmp(key->name, key_name) == 0) {
            return key;
        }
        key = key->next;
    }
    
    return NULL;
}

char* ipns_manager_publish(IpnsManager* manager, const IpfsCid* cid, const char* key_name) {
    if (!manager || !cid) return NULL;
    
    IpnsKey* key = key_name ? ipns_manager_get_key(manager, key_name) : manager->default_key;
    if (!key) return NULL;
    
    // Create IPNS record
    IpnsRecord* record = ipns_record_create(key->id, cid);
    if (!record) return NULL;
    
    record->signing_key = key;
    record->auto_republish = manager->auto_republish_enabled;
    record->republish_interval = manager->default_ttl / 2; // Republish at half TTL
    
    // Publish via IPFS client
    CURL* curl = curl_easy_init();
    if (!curl) {
        ipns_record_free(record);
        return NULL;
    }
    
    char url[512];
    snprintf(url, sizeof(url), "%s/api/v0/name/publish?arg=%s&key=%s&lifetime=%ds",
             manager->ipfs_client->api_url, cid->hash, key->name, manager->default_ttl);
    
    HTTPResponse response = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)manager->resolution_timeout);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || !response.data) {
        ipns_record_free(record);
        free(response.data);
        return NULL;
    }
    
    // Parse response to get IPNS name
    json_object* root = json_tokener_parse(response.data);
    json_object* name_obj;
    char* ipns_name = NULL;
    
    if (root && json_object_object_get_ex(root, "Name", &name_obj)) {
        const char* name_str = json_object_get_string(name_obj);
        if (name_str) {
            ipns_name = strdup(name_str);
            free(record->ipns_name);
            record->ipns_name = strdup(name_str);
        }
    }
    
    // Add record to manager
    if (ipns_name) {
        ipns_manager_add_record(manager, record);
    } else {
        ipns_record_free(record);
    }
    
    json_object_put(root);
    free(response.data);
    
    return ipns_name;
}

IpfsCid* ipns_manager_resolve(IpnsManager* manager, const char* ipns_name) {
    if (!manager || !ipns_name) return NULL;
    
    // Check cache first
    IpfsCid* cached = ipns_manager_get_cached_record(manager, ipns_name);
    if (cached) {
        return ipfs_cid_clone(cached);
    }
    
    // Resolve via IPFS client
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    
    char url[512];
    snprintf(url, sizeof(url), "%s/api/v0/name/resolve?arg=%s",
             manager->ipfs_client->api_url, ipns_name);
    
    HTTPResponse response = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)manager->resolution_timeout);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || !response.data) {
        free(response.data);
        return NULL;
    }
    
    // Parse response
    json_object* root = json_tokener_parse(response.data);
    json_object* path_obj;
    IpfsCid* cid = NULL;
    
    if (root && json_object_object_get_ex(root, "Path", &path_obj)) {
        const char* path_str = json_object_get_string(path_obj);
        if (path_str && strncmp(path_str, "/ipfs/", 6) == 0) {
            cid = ipfs_cid_from_string(path_str + 6);
            
            // Cache the result
            if (cid) {
                ipns_manager_cache_record(manager, ipns_name, cid, manager->cache_duration);
            }
        }
    }
    
    json_object_put(root);
    free(response.data);
    
    return cid;
}

bool ipns_manager_cache_record(IpnsManager* manager, const char* ipns_name, 
                             const IpfsCid* cid, int ttl_seconds) {
    if (!manager || !ipns_name || !cid) return false;
    
    IpnsRecord* record = ipns_manager_get_record(manager, ipns_name);
    if (!record) {
        record = ipns_record_create(ipns_name, cid);
        if (!record) return false;
        ipns_manager_add_record(manager, record);
    }
    
    // Update cache information
    if (record->cached_cid) {
        ipfs_cid_free(record->cached_cid);
    }
    record->cached_cid = ipfs_cid_clone(cid);
    record->cache_expires = time(NULL) + ttl_seconds;
    record->last_resolved = time(NULL);
    
    return true;
}

IpfsCid* ipns_manager_get_cached_record(IpnsManager* manager, const char* ipns_name) {
    if (!manager || !ipns_name) return NULL;
    
    IpnsRecord* record = ipns_manager_get_record(manager, ipns_name);
    if (!record || !record->cached_cid) return NULL;
    
    // Check if cache is still valid
    if (time(NULL) > record->cache_expires) {
        ipfs_cid_free(record->cached_cid);
        record->cached_cid = NULL;
        return NULL;
    }
    
    return record->cached_cid;
}

IpnsRecord* ipns_manager_get_record(IpnsManager* manager, const char* ipns_name) {
    if (!manager || !ipns_name) return NULL;
    
    IpnsRecord* record = manager->records;
    while (record) {
        if (strcmp(record->ipns_name, ipns_name) == 0) {
            return record;
        }
        record = record->next;
    }
    
    return NULL;
}

bool ipns_manager_add_record(IpnsManager* manager, IpnsRecord* record) {
    if (!manager || !record) return false;
    
    // Add to beginning of list
    record->next = manager->records;
    manager->records = record;
    manager->record_count++;
    
    return true;
}

IpnsSubscription* ipns_manager_subscribe(IpnsManager* manager, const char* ipns_name,
                                       void (*callback)(const char* ipns_name, const IpfsCid* new_cid, void* user_data),
                                       void* user_data) {
    if (!manager || !ipns_name || !callback) return NULL;
    
    IpnsSubscription* subscription = ipns_subscription_create(ipns_name);
    if (!subscription) return NULL;
    
    subscription->update_callback = callback;
    subscription->user_data = user_data;
    subscription->is_active = true;
    subscription->subscribed_at = time(NULL);
    subscription->check_interval = 60; // Check every minute by default
    
    // Get current CID for comparison
    subscription->last_known_cid = ipns_manager_resolve(manager, ipns_name);
    
    // Add to manager's subscription list
    subscription->next = manager->subscriptions;
    manager->subscriptions = subscription;
    
    // Start monitoring thread if not already running
    if (!manager->subscription_thread_active) {
        ipns_manager_start_subscription_monitor(manager);
    }
    
    return subscription;
}

bool ipns_manager_start_subscription_monitor(IpnsManager* manager) {
    if (!manager || manager->subscription_thread_active) return false;
    
    if (!g_subscription_monitor) {
        g_subscription_monitor = calloc(1, sizeof(SubscriptionMonitor));
        if (!g_subscription_monitor) return false;
    }
    
    g_subscription_monitor->manager = manager;
    g_subscription_monitor->should_stop = false;
    
    int result = pthread_create(&g_subscription_monitor->thread, NULL, 
                               subscription_monitor_thread, g_subscription_monitor);
    if (result != 0) return false;
    
    manager->subscription_thread_active = true;
    return true;
}

bool ipns_manager_stop_subscription_monitor(IpnsManager* manager) {
    if (!manager || !manager->subscription_thread_active || !g_subscription_monitor) {
        return false;
    }
    
    g_subscription_monitor->should_stop = true;
    pthread_join(g_subscription_monitor->thread, NULL);
    
    free(g_subscription_monitor);
    g_subscription_monitor = NULL;
    
    manager->subscription_thread_active = false;
    return true;
}

// Key operations
IpnsKey* ipns_key_create(const char* name) {
    if (!name) return NULL;
    
    IpnsKey* key = calloc(1, sizeof(IpnsKey));
    if (!key) return NULL;
    
    key->name = strdup(name);
    if (!key->name) {
        free(key);
        return NULL;
    }
    
    key->created_at = time(NULL);
    return key;
}

void ipns_key_free(IpnsKey* key) {
    if (!key) return;
    
    free(key->name);
    free(key->id);
    free(key->public_key);
    free(key->private_key);
    free(key);
}

bool ipns_key_generate_keypair(IpnsKey* key) {
    if (!key) return false;
    
    // Generate Ed25519 keypair using OpenSSL
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!ctx) return false;
    
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    
    EVP_PKEY* pkey = NULL;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    
    EVP_PKEY_CTX_free(ctx);
    
    // Extract public key
    size_t pub_len = 0;
    if (EVP_PKEY_get_raw_public_key(pkey, NULL, &pub_len) <= 0) {
        EVP_PKEY_free(pkey);
        return false;
    }
    
    unsigned char* pub_key = malloc(pub_len);
    if (!pub_key || EVP_PKEY_get_raw_public_key(pkey, pub_key, &pub_len) <= 0) {
        free(pub_key);
        EVP_PKEY_free(pkey);
        return false;
    }
    
    // Extract private key
    size_t priv_len = 0;
    if (EVP_PKEY_get_raw_private_key(pkey, NULL, &priv_len) <= 0) {
        free(pub_key);
        EVP_PKEY_free(pkey);
        return false;
    }
    
    unsigned char* priv_key = malloc(priv_len);
    if (!priv_key || EVP_PKEY_get_raw_private_key(pkey, priv_key, &priv_len) <= 0) {
        free(pub_key);
        free(priv_key);
        EVP_PKEY_free(pkey);
        return false;
    }
    
    // Encode keys as base64 (simplified)
    key->public_key = malloc(pub_len * 2 + 1);
    key->private_key = malloc(priv_len * 2 + 1);
    
    if (!key->public_key || !key->private_key) {
        free(pub_key);
        free(priv_key);
        EVP_PKEY_free(pkey);
        return false;
    }
    
    // Simple hex encoding (in practice would use proper base64)
    for (size_t i = 0; i < pub_len; i++) {
        sprintf(key->public_key + i * 2, "%02x", pub_key[i]);
    }
    
    for (size_t i = 0; i < priv_len; i++) {
        sprintf(key->private_key + i * 2, "%02x", priv_key[i]);
    }
    
    // Generate key ID
    key->id = generate_key_id(key->public_key);
    
    free(pub_key);
    free(priv_key);
    EVP_PKEY_free(pkey);
    
    return true;
}

// Record operations
IpnsRecord* ipns_record_create(const char* ipns_name, const IpfsCid* cid) {
    if (!ipns_name || !cid) return NULL;
    
    IpnsRecord* record = calloc(1, sizeof(IpnsRecord));
    if (!record) return NULL;
    
    record->ipns_name = strdup(ipns_name);
    record->current_cid = ipfs_cid_clone(cid);
    
    if (!record->ipns_name || !record->current_cid) {
        ipns_record_free(record);
        return NULL;
    }
    
    record->created_at = time(NULL);
    record->updated_at = time(NULL);
    record->sequence_number = 1;
    record->is_resolvable = true;
    record->resolution_confidence = 1.0f;
    
    return record;
}

void ipns_record_free(IpnsRecord* record) {
    if (!record) return;
    
    free(record->ipns_name);
    free(record->value_path);
    ipfs_cid_free(record->current_cid);
    ipfs_cid_free(record->cached_cid);
    free(record);
}

// Subscription operations
IpnsSubscription* ipns_subscription_create(const char* ipns_name) {
    if (!ipns_name) return NULL;
    
    IpnsSubscription* subscription = calloc(1, sizeof(IpnsSubscription));
    if (!subscription) return NULL;
    
    subscription->ipns_name = strdup(ipns_name);
    if (!subscription->ipns_name) {
        free(subscription);
        return NULL;
    }
    
    subscription->check_interval = 60; // Default 1 minute
    subscription->notify_on_error = true;
    subscription->cache_updates = true;
    
    return subscription;
}

void ipns_subscription_free(IpnsSubscription* subscription) {
    if (!subscription) return;
    
    free(subscription->ipns_name);
    ipfs_cid_free(subscription->last_known_cid);
    free(subscription);
}

bool ipns_subscription_check_update(IpnsSubscription* subscription, IpnsManager* manager) {
    if (!subscription || !manager) return false;
    
    IpfsCid* current_cid = ipns_manager_resolve(manager, subscription->ipns_name);
    if (!current_cid) {
        subscription->consecutive_failures++;
        return false;
    }
    
    // Check if CID has changed
    bool changed = false;
    if (!subscription->last_known_cid) {
        changed = true;
    } else if (!ipfs_cid_equals(subscription->last_known_cid, current_cid)) {
        changed = true;
    }
    
    if (changed) {
        ipfs_cid_free(subscription->last_known_cid);
        subscription->last_known_cid = ipfs_cid_clone(current_cid);
        subscription->consecutive_failures = 0;
        
        // Notify callback
        if (subscription->update_callback) {
            subscription->update_callback(subscription->ipns_name, current_cid, 
                                        subscription->user_data);
        }
    }
    
    ipfs_cid_free(current_cid);
    return changed;
}