#include "package/ipfs_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

// HTTP response structure for libcurl
typedef struct {
    char* data;
    size_t size;
} HttpResponse;

// Write callback for libcurl
static size_t write_callback(void* contents, size_t size, size_t nmemb, HttpResponse* response) {
    size_t total_size = size * nmemb;
    char* new_data = realloc(response->data, response->size + total_size + 1);
    
    if (!new_data) {
        return 0; // Out of memory
    }
    
    response->data = new_data;
    memcpy(&(response->data[response->size]), contents, total_size);
    response->size += total_size;
    response->data[response->size] = '\0';
    
    return total_size;
}

// Progress callback for downloads
static int progress_callback(void* user_data, curl_off_t download_total, curl_off_t download_now,
                           curl_off_t upload_total, curl_off_t upload_now) {
    IpfsProgressCallback callback = (IpfsProgressCallback)user_data;
    if (callback && download_total > 0) {
        callback((size_t)download_now, (size_t)download_total, NULL);
    }
    return 0;
}

// Client creation and lifecycle
IpfsClient* ipfs_client_create(const IpfsClientConfig* config) {
    if (!config) return NULL;
    
    IpfsClient* client = calloc(1, sizeof(IpfsClient));
    if (!client) return NULL;
    
    // Copy configuration
    client->config = calloc(1, sizeof(IpfsClientConfig));
    if (!client->config) {
        free(client);
        return NULL;
    }
    
    *client->config = *config;
    
    // Initialize curl
    client->curl_handle = curl_easy_init();
    if (!client->curl_handle) {
        free(client->config);
        free(client);
        return NULL;
    }
    
    // Set default cache directory
    if (config->cache_dir) {
        client->cache_dir = strdup(config->cache_dir);
    } else {
        client->cache_dir = strdup("/tmp/goo_ipfs_cache");
    }
    
    return client;
}

void ipfs_client_free(IpfsClient* client) {
    if (!client) return;
    
    if (client->curl_handle) {
        curl_easy_cleanup(client->curl_handle);
    }
    
    free(client->cache_dir);
    free(client->config);
    free(client);
}

bool ipfs_client_initialize(IpfsClient* client) {
    if (!client) return false;
    
    // Ensure cache directory exists
    if (!ipfs_ensure_directory(client->cache_dir)) {
        return false;
    }
    
    // Auto-start daemon if configured
    if (client->config->auto_start_daemon) {
        if (!ipfs_daemon_start(client)) {
            // Continue without local daemon, use gateways
        }
    }
    
    // Check daemon status
    client->is_daemon_running = ipfs_daemon_is_running(client);
    
    // Select best gateway
    client->active_gateway = ipfs_gateway_select_best(client);
    
    return true;
}

// IPFS daemon management
bool ipfs_daemon_start(IpfsClient* client) {
    if (!client) return false;
    
    // Try to start IPFS daemon
    int result = system("ipfs daemon &");
    if (result == 0) {
        // Wait a moment for daemon to start
        sleep(2);
        client->is_daemon_running = ipfs_daemon_is_running(client);
        return client->is_daemon_running;
    }
    
    return false;
}

bool ipfs_daemon_stop(IpfsClient* client) {
    if (!client) return false;
    
    int result = system("pkill ipfs");
    if (result == 0) {
        client->is_daemon_running = false;
        return true;
    }
    
    return false;
}

bool ipfs_daemon_is_running(IpfsClient* client) {
    if (!client) return false;
    
    // Try to connect to local API
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    HttpResponse response = {0};
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:5001/api/v0/version");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    bool is_running = (res == CURLE_OK && response.data != NULL);
    
    free(response.data);
    return is_running;
}

// Content operations
IpfsCid* ipfs_add_file(IpfsClient* client, const char* filepath) {
    if (!client || !filepath) return NULL;
    
    if (!client->is_daemon_running) {
        // TODO: Use gateway for upload if local daemon not available
        return NULL;
    }
    
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    
    HttpResponse response = {0};
    
    // Prepare form data
    curl_mime* form = curl_mime_init(curl);
    curl_mimepart* field = curl_mime_addpart(form);
    curl_mime_name(field, "file");
    curl_mime_filedata(field, filepath);
    
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:5001/api/v0/add");
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    
    CURLcode res = curl_easy_perform(curl);
    
    curl_mime_free(form);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || !response.data) {
        free(response.data);
        return NULL;
    }
    
    // Parse JSON response
    json_object* json = json_tokener_parse(response.data);
    free(response.data);
    
    if (!json) return NULL;
    
    json_object* hash_obj;
    if (!json_object_object_get_ex(json, "Hash", &hash_obj)) {
        json_object_put(json);
        return NULL;
    }
    
    const char* hash = json_object_get_string(hash_obj);
    IpfsCid* cid = ipfs_cid_create(hash);
    
    json_object_put(json);
    return cid;
}

bool ipfs_get_file(IpfsClient* client, const IpfsCid* cid, const char* output_path) {
    if (!client || !cid || !output_path) return false;
    
    // Try local daemon first
    if (client->is_daemon_running) {
        char url[512];
        snprintf(url, sizeof(url), "http://127.0.0.1:8080/ipfs/%s", cid->hash);
        
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        FILE* fp = fopen(output_path, "wb");
        if (!fp) {
            curl_easy_cleanup(curl);
            return false;
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        CURLcode res = curl_easy_perform(curl);
        
        fclose(fp);
        curl_easy_cleanup(curl);
        
        if (res == CURLE_OK) {
            return true;
        }
    }
    
    // Fallback to public gateways
    const char* gateways[] = {
        "https://ipfs.io",
        "https://dweb.link",
        "https://cf-ipfs.com",
        NULL
    };
    
    for (int i = 0; gateways[i]; i++) {
        char url[512];
        snprintf(url, sizeof(url), "%s/ipfs/%s", gateways[i], cid->hash);
        
        CURL* curl = curl_easy_init();
        if (!curl) continue;
        
        FILE* fp = fopen(output_path, "wb");
        if (!fp) {
            curl_easy_cleanup(curl);
            continue;
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        
        CURLcode res = curl_easy_perform(curl);
        
        fclose(fp);
        curl_easy_cleanup(curl);
        
        if (res == CURLE_OK) {
            return true;
        }
        
        // Remove failed partial download
        unlink(output_path);
    }
    
    return false;
}

// Pinning operations
bool ipfs_pin_add(IpfsClient* client, const IpfsCid* cid) {
    if (!client || !cid || !client->is_daemon_running) return false;
    
    char url[512];
    snprintf(url, sizeof(url), "http://127.0.0.1:5001/api/v0/pin/add?arg=%s", cid->hash);
    
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    HttpResponse response = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    bool success = (res == CURLE_OK);
    free(response.data);
    return success;
}

bool ipfs_pin_remove(IpfsClient* client, const IpfsCid* cid) {
    if (!client || !cid || !client->is_daemon_running) return false;
    
    char url[512];
    snprintf(url, sizeof(url), "http://127.0.0.1:5001/api/v0/pin/rm?arg=%s", cid->hash);
    
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    HttpResponse response = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    bool success = (res == CURLE_OK);
    free(response.data);
    return success;
}

// Gateway operations
IpfsGateway* ipfs_gateway_create(const char* url, const char* name) {
    if (!url) return NULL;
    
    IpfsGateway* gateway = calloc(1, sizeof(IpfsGateway));
    if (!gateway) return NULL;
    
    gateway->url = strdup(url);
    gateway->name = name ? strdup(name) : strdup("Unknown");
    gateway->is_local = (strstr(url, "127.0.0.1") != NULL || strstr(url, "localhost") != NULL);
    gateway->response_time = -1.0f;
    gateway->reliability = 0.0f;
    gateway->last_check = 0;
    
    return gateway;
}

void ipfs_gateway_free(IpfsGateway* gateway) {
    if (!gateway) return;
    
    free(gateway->url);
    free(gateway->name);
    free(gateway);
}

bool ipfs_gateway_health_check(IpfsClient* client, IpfsGateway* gateway) {
    if (!client || !gateway) return false;
    
    // Test with a known IPFS hash (empty directory)
    const char* test_hash = "QmUNLLsPACCz1vLxQVkXqqLX5R1X345qqfHbsf67hvA3Nn";
    char url[512];
    snprintf(url, sizeof(url), "%s/ipfs/%s", gateway->url, test_hash);
    
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    HttpResponse response = {0};
    
    // Record start time
    clock_t start_time = clock();
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // HEAD request only
    
    CURLcode res = curl_easy_perform(curl);
    
    // Calculate response time
    clock_t end_time = clock();
    gateway->response_time = ((float)(end_time - start_time) / CLOCKS_PER_SEC) * 1000.0f;
    
    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    
    curl_easy_cleanup(curl);
    free(response.data);
    
    bool is_healthy = (res == CURLE_OK && response_code == 200);
    gateway->last_check = time(NULL);
    
    // Update reliability score
    if (is_healthy) {
        gateway->reliability = gateway->reliability * 0.9f + 0.1f;
    } else {
        gateway->reliability = gateway->reliability * 0.9f;
    }
    
    return is_healthy;
}

IpfsGateway* ipfs_gateway_select_best(IpfsClient* client) {
    if (!client) return NULL;
    
    // Default gateways if none configured
    if (!client->config->gateways) {
        IpfsGateway* gateway = ipfs_gateway_create("https://ipfs.io", "IPFS.io");
        return gateway;
    }
    
    IpfsGateway* best_gateway = NULL;
    float best_score = -1.0f;
    
    for (IpfsGateway* gateway = client->config->gateways; gateway; gateway = gateway->next) {
        // Health check if not done recently
        if (time(NULL) - gateway->last_check > 300) { // 5 minutes
            ipfs_gateway_health_check(client, gateway);
        }
        
        // Calculate score (lower response time + higher reliability)
        float score = gateway->reliability;
        if (gateway->response_time > 0) {
            score = score / (1.0f + gateway->response_time / 1000.0f);
        }
        
        if (score > best_score) {
            best_score = score;
            best_gateway = gateway;
        }
    }
    
    return best_gateway;
}

// CID operations
IpfsCid* ipfs_cid_create(const char* hash) {
    if (!hash) return NULL;
    
    IpfsCid* cid = calloc(1, sizeof(IpfsCid));
    if (!cid) return NULL;
    
    cid->hash = strdup(hash);
    cid->version = (hash[0] == 'Q') ? 0 : 1; // Simple heuristic
    
    return cid;
}

void ipfs_cid_free(IpfsCid* cid) {
    if (!cid) return;
    
    free(cid->hash);
    free(cid->multihash);
    free(cid->codec);
    free(cid);
}

bool ipfs_cid_validate(const char* cid_string) {
    if (!cid_string) return false;
    
    size_t len = strlen(cid_string);
    
    // Basic validation
    if (len < 10) return false;
    
    // CIDv0 starts with 'Qm' and is base58
    if (len == 46 && cid_string[0] == 'Q' && cid_string[1] == 'm') {
        return true;
    }
    
    // CIDv1 is typically longer and starts with different characters
    if (len > 46) {
        return true; // More sophisticated validation would be needed
    }
    
    return false;
}

char* ipfs_cid_to_string(const IpfsCid* cid) {
    if (!cid || !cid->hash) return NULL;
    return strdup(cid->hash);
}

// Package-specific operations
IpfsPackageManifest* ipfs_package_publish(IpfsClient* client, const char* package_dir) {
    if (!client || !package_dir) return NULL;
    
    // Add entire directory to IPFS
    IpfsCid* content_cid = ipfs_add_directory(client, package_dir);
    if (!content_cid) return NULL;
    
    // Create manifest
    IpfsPackageManifest* manifest = calloc(1, sizeof(IpfsPackageManifest));
    if (!manifest) {
        ipfs_cid_free(content_cid);
        return NULL;
    }
    
    manifest->content_cid = content_cid;
    manifest->published_at = time(NULL);
    manifest->total_size = 0; // TODO: Calculate actual size
    manifest->pin_count = 1;  // At least this node
    
    // TODO: Extract name and version from goo.mod in package_dir
    manifest->name = strdup("unknown");
    manifest->version = strdup("0.0.0");
    
    return manifest;
}

void ipfs_package_manifest_free(IpfsPackageManifest* manifest) {
    if (!manifest) return;
    
    free(manifest->name);
    free(manifest->version);
    ipfs_cid_free(manifest->content_cid);
    ipfs_cid_free(manifest->source_cid);
    ipfs_cid_free(manifest->docs_cid);
    ipfs_cid_free(manifest->tests_cid);
    free(manifest->publisher_peer_id);
    
    if (manifest->signatures) {
        for (size_t i = 0; i < manifest->signature_count; i++) {
            free(manifest->signatures[i]);
        }
        free(manifest->signatures);
    }
    
    free(manifest);
}

// Configuration management
IpfsClientConfig* ipfs_config_create_default(void) {
    IpfsClientConfig* config = calloc(1, sizeof(IpfsClientConfig));
    if (!config) return NULL;
    
    config->cache_dir = strdup("/tmp/goo_ipfs_cache");
    config->cache_size_limit = 1024 * 1024 * 1024; // 1GB
    config->gateway_timeout = 30;
    config->max_concurrent_downloads = 4;
    config->auto_start_daemon = true;
    config->enable_p2p_sharing = true;
    config->verify_content = true;
    
    // Add default gateways
    config->gateways = ipfs_gateway_create("https://ipfs.io", "IPFS.io");
    config->gateways->next = ipfs_gateway_create("https://dweb.link", "Dweb.link");
    config->gateways->next->next = ipfs_gateway_create("https://cf-ipfs.com", "Cloudflare");
    
    return config;
}

void ipfs_config_free(IpfsClientConfig* config) {
    if (!config) return;
    
    free(config->cache_dir);
    
    IpfsGateway* gateway = config->gateways;
    while (gateway) {
        IpfsGateway* next = gateway->next;
        ipfs_gateway_free(gateway);
        gateway = next;
    }
    
    free(config);
}

// Utility functions
bool ipfs_ensure_directory(const char* path) {
    if (!path) return false;
    
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        return mkdir(path, 0755) == 0;
    }
    
    return S_ISDIR(st.st_mode);
}

char* ipfs_get_cache_path(IpfsClient* client, const IpfsCid* cid) {
    if (!client || !cid) return NULL;
    
    size_t path_len = strlen(client->cache_dir) + strlen(cid->hash) + 10;
    char* cache_path = malloc(path_len);
    if (!cache_path) return NULL;
    
    snprintf(cache_path, path_len, "%s/%s", client->cache_dir, cid->hash);
    return cache_path;
}

// Error handling
const char* ipfs_error_string(IpfsError error) {
    switch (error) {
        case IPFS_SUCCESS: return "Success";
        case IPFS_ERROR_INVALID_CID: return "Invalid CID";
        case IPFS_ERROR_NETWORK: return "Network error";
        case IPFS_ERROR_TIMEOUT: return "Request timeout";
        case IPFS_ERROR_NOT_FOUND: return "Content not found";
        case IPFS_ERROR_PERMISSION_DENIED: return "Permission denied";
        case IPFS_ERROR_INSUFFICIENT_STORAGE: return "Insufficient storage";
        case IPFS_ERROR_DAEMON_NOT_RUNNING: return "IPFS daemon not running";
        case IPFS_ERROR_INVALID_CONFIG: return "Invalid configuration";
        case IPFS_ERROR_VERIFICATION_FAILED: return "Content verification failed";
        case IPFS_ERROR_GATEWAY_UNAVAILABLE: return "Gateway unavailable";
        default: return "Unknown error";
    }
}

// Stub implementations for missing functions
IpfsCid* ipfs_add_directory(IpfsClient* client, const char* dirpath) {
    // TODO: Implement directory upload
    return NULL;
}

IpfsCid* ipfs_add_data(IpfsClient* client, const void* data, size_t size) {
    // TODO: Implement data upload
    return NULL;
}

bool ipfs_get_directory(IpfsClient* client, const IpfsCid* cid, const char* output_dir) {
    // TODO: Implement directory download
    return false;
}

char* ipfs_cat(IpfsClient* client, const IpfsCid* cid, size_t* size) {
    // TODO: Implement cat operation
    return NULL;
}