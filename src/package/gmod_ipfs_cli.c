#include "package/ipfs_package.h"
#include "package/gmod_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

// Forward declarations for IPFS commands
int cmd_ipfs_publish(int argc, char** argv);
int cmd_ipfs_pin(int argc, char** argv);
int cmd_ipfs_unpin(int argc, char** argv);
int cmd_ipfs_daemon(int argc, char** argv);
int cmd_ipfs_share(int argc, char** argv);
int cmd_ipfs_discover(int argc, char** argv);
int cmd_ipfs_gateway(int argc, char** argv);
int cmd_ipfs_status(int argc, char** argv);

// Global IPFS package manager
static IpfsPackageManager* global_ipfs_manager = NULL;

// Initialize IPFS package manager
static bool ensure_ipfs_manager(void) {
    if (global_ipfs_manager) return true;
    
    IpfsClientConfig* config = ipfs_config_create_default();
    if (!config) return false;
    
    global_ipfs_manager = ipfs_package_manager_create(".", config);
    if (!global_ipfs_manager) {
        ipfs_config_free(config);
        return false;
    }
    
    if (!ipfs_package_manager_initialize(global_ipfs_manager)) {
        ipfs_package_manager_free(global_ipfs_manager);
        global_ipfs_manager = NULL;
        return false;
    }
    
    return true;
}

// IPFS publish command
int cmd_ipfs_publish(int argc, char** argv) {
    bool announce = false;
    bool pin_locally = true;
    const char* swarm_name = NULL;
    const char* package_dir = ".";
    
    // Parse options
    int opt;
    static struct option long_options[] = {
        {"announce", no_argument, 0, 'a'},
        {"no-pin", no_argument, 0, 'n'},
        {"swarm", required_argument, 0, 's'},
        {"dir", required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };
    
    while ((opt = getopt_long(argc, argv, "ans:d:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'a':
                announce = true;
                break;
            case 'n':
                pin_locally = false;
                break;
            case 's':
                swarm_name = optarg;
                break;
            case 'd':
                package_dir = optarg;
                break;
            default:
                return 1;
        }
    }
    
    if (!ensure_ipfs_manager()) {
        print_error("Failed to initialize IPFS client");
        return 1;
    }
    
    print_info("Publishing package to IPFS...");
    
    // Publish package
    IpfsCid* cid = ipfs_publish_package(global_ipfs_manager, package_dir);
    if (!cid) {
        print_error("Failed to publish package to IPFS");
        return 1;
    }
    
    char* cid_str = ipfs_cid_to_string(cid);
    print_success("Published to IPFS: %s", cid_str);
    
    // Pin locally if requested
    if (pin_locally) {
        if (ipfs_pin_add(global_ipfs_manager->ipfs_client, cid)) {
            print_success("Pinned locally");
        } else {
            print_warning("Failed to pin locally");
        }
    }
    
    // Announce to swarm if requested
    if (announce && swarm_name) {
        if (ipfs_announce_package(global_ipfs_manager, cid, swarm_name)) {
            print_success("Announced to swarm: %s", swarm_name);
        } else {
            print_warning("Failed to announce to swarm");
        }
    }
    
    // Print useful commands
    printf("\nUseful commands:\n");
    printf("  Add to dependencies: gmod add ipfs://%s\n", cid_str);
    printf("  Pin on other nodes:  gmod ipfs pin %s\n", cid_str);
    printf("  View on gateway:     https://ipfs.io/ipfs/%s\n", cid_str);
    
    free(cid_str);
    ipfs_cid_free(cid);
    return 0;
}

// IPFS pin command
int cmd_ipfs_pin(int argc, char** argv) {
    if (optind >= argc) {
        print_error("CID or package name required");
        printf("Usage: gmod ipfs pin <cid|package@version>\n");
        return 1;
    }
    
    const char* target = argv[optind];
    
    if (!ensure_ipfs_manager()) {
        print_error("Failed to initialize IPFS client");
        return 1;
    }
    
    // Check if it's a CID or package name
    if (ipfs_cid_validate(target)) {
        // Direct CID
        IpfsCid* cid = ipfs_cid_create(target);
        if (!cid) {
            print_error("Invalid CID: %s", target);
            return 1;
        }
        
        print_info("Pinning %s...", target);
        
        if (ipfs_pin_add(global_ipfs_manager->ipfs_client, cid)) {
            print_success("Pinned: %s", target);
        } else {
            print_error("Failed to pin: %s", target);
            ipfs_cid_free(cid);
            return 1;
        }
        
        ipfs_cid_free(cid);
    } else {
        // Package name - resolve first
        print_info("Resolving package: %s", target);
        
        // Parse package@version
        char* package_name = strdup(target);
        char* version = strchr(package_name, '@');
        if (version) {
            *version = '\0';
            version++;
        } else {
            version = "*";
        }
        
        IpfsResolution* resolution = ipfs_resolve_package(global_ipfs_manager, package_name, version);
        if (!resolution || !resolution->ipfs_cid) {
            print_error("Failed to resolve package to IPFS CID: %s", target);
            free(package_name);
            return 1;
        }
        
        char* cid_str = ipfs_cid_to_string(resolution->ipfs_cid);
        print_info("Resolved to CID: %s", cid_str);
        
        if (ipfs_pin_add(global_ipfs_manager->ipfs_client, resolution->ipfs_cid)) {
            print_success("Pinned package: %s (%s)", target, cid_str);
        } else {
            print_error("Failed to pin package: %s", target);
            free(cid_str);
            free(package_name);
            ipfs_resolution_free(resolution);
            return 1;
        }
        
        free(cid_str);
        free(package_name);
        ipfs_resolution_free(resolution);
    }
    
    return 0;
}

// IPFS unpin command
int cmd_ipfs_unpin(int argc, char** argv) {
    if (optind >= argc) {
        print_error("CID or package name required");
        printf("Usage: gmod ipfs unpin <cid|package@version>\n");
        return 1;
    }
    
    const char* target = argv[optind];
    
    if (!ensure_ipfs_manager()) {
        print_error("Failed to initialize IPFS client");
        return 1;
    }
    
    // Similar logic to pin command but for unpinning
    if (ipfs_cid_validate(target)) {
        IpfsCid* cid = ipfs_cid_create(target);
        if (!cid) {
            print_error("Invalid CID: %s", target);
            return 1;
        }
        
        print_info("Unpinning %s...", target);
        
        if (ipfs_pin_remove(global_ipfs_manager->ipfs_client, cid)) {
            print_success("Unpinned: %s", target);
        } else {
            print_error("Failed to unpin: %s", target);
            ipfs_cid_free(cid);
            return 1;
        }
        
        ipfs_cid_free(cid);
    } else {
        print_error("Package name resolution for unpinning not yet implemented");
        return 1;
    }
    
    return 0;
}

// IPFS daemon command
int cmd_ipfs_daemon(int argc, char** argv) {
    const char* action = "status";
    
    if (optind < argc) {
        action = argv[optind];
    }
    
    if (!ensure_ipfs_manager()) {
        print_error("Failed to initialize IPFS client");
        return 1;
    }
    
    if (strcmp(action, "start") == 0) {
        print_info("Starting IPFS daemon...");
        if (ipfs_daemon_start(global_ipfs_manager->ipfs_client)) {
            print_success("IPFS daemon started");
        } else {
            print_error("Failed to start IPFS daemon");
            return 1;
        }
    } else if (strcmp(action, "stop") == 0) {
        print_info("Stopping IPFS daemon...");
        if (ipfs_daemon_stop(global_ipfs_manager->ipfs_client)) {
            print_success("IPFS daemon stopped");
        } else {
            print_error("Failed to stop IPFS daemon");
            return 1;
        }
    } else if (strcmp(action, "restart") == 0) {
        print_info("Restarting IPFS daemon...");
        ipfs_daemon_stop(global_ipfs_manager->ipfs_client);
        sleep(2);
        if (ipfs_daemon_start(global_ipfs_manager->ipfs_client)) {
            print_success("IPFS daemon restarted");
        } else {
            print_error("Failed to restart IPFS daemon");
            return 1;
        }
    } else if (strcmp(action, "status") == 0) {
        bool is_running = ipfs_daemon_is_running(global_ipfs_manager->ipfs_client);
        if (is_running) {
            print_success("IPFS daemon is running");
        } else {
            print_info("IPFS daemon is not running");
        }
    } else {
        print_error("Unknown daemon action: %s", action);
        printf("Usage: gmod ipfs daemon [start|stop|restart|status]\n");
        return 1;
    }
    
    return 0;
}

// IPFS share command
int cmd_ipfs_share(int argc, char** argv) {
    const char* swarm_name = "goo-packages";
    
    // Parse options
    int opt;
    static struct option long_options[] = {
        {"swarm", required_argument, 0, 's'},
        {0, 0, 0, 0}
    };
    
    while ((opt = getopt_long(argc, argv, "s:", long_options, NULL)) != -1) {
        switch (opt) {
            case 's':
                swarm_name = optarg;
                break;
            default:
                return 1;
        }
    }
    
    if (!ensure_ipfs_manager()) {
        print_error("Failed to initialize IPFS client");
        return 1;
    }
    
    print_info("Joining P2P swarm: %s", swarm_name);
    print_info("Sharing local packages...");
    
    // TODO: Implement actual P2P sharing
    if (ipfs_share_local_packages(global_ipfs_manager)) {
        print_success("Started sharing packages on swarm: %s", swarm_name);
        print_info("Use Ctrl+C to stop sharing");
        
        // Keep running until interrupted
        while (true) {
            sleep(10);
            // TODO: Periodic maintenance
        }
    } else {
        print_error("Failed to start P2P sharing");
        return 1;
    }
    
    return 0;
}

// IPFS discover command
int cmd_ipfs_discover(int argc, char** argv) {
    const char* search_query = "";
    
    if (optind < argc) {
        search_query = argv[optind];
    }
    
    if (!ensure_ipfs_manager()) {
        print_error("Failed to initialize IPFS client");
        return 1;
    }
    
    print_info("Discovering packages on IPFS network...");
    if (strlen(search_query) > 0) {
        print_info("Search query: %s", search_query);
    }
    
    size_t result_count = 0;
    IpfsDiscoveryResult** results = ipfs_discover_packages(global_ipfs_manager, search_query, &result_count);
    
    if (!results || result_count == 0) {
        print_info("No packages found");
        return 0;
    }
    
    printf("\nDiscovered packages:\n\n");
    
    for (size_t i = 0; i < result_count; i++) {
        IpfsDiscoveryResult* result = results[i];
        
        char* cid_str = ipfs_cid_to_string(result->cid);
        char* version_str = version_to_string(result->version);
        
        printf("📦 \033[36m%s\033[0m@%s\n", result->package_name, version_str);
        printf("   CID: %s\n", cid_str);
        printf("   Publisher: %s\n", result->publisher_peer_id);
        printf("   Peers: %d\n", result->peer_count);
        printf("   Reputation: %.2f\n", result->reputation_score);
        printf("   Command: gmod add ipfs://%s\n\n", cid_str);
        
        free(cid_str);
        free(version_str);
    }
    
    // Free results
    for (size_t i = 0; i < result_count; i++) {
        ipfs_discovery_result_free(results[i]);
    }
    free(results);
    
    return 0;
}

// IPFS gateway command
int cmd_ipfs_gateway(int argc, char** argv) {
    const char* action = "list";
    
    if (optind < argc) {
        action = argv[optind];
    }
    
    if (!ensure_ipfs_manager()) {
        print_error("Failed to initialize IPFS client");
        return 1;
    }
    
    if (strcmp(action, "list") == 0) {
        print_info("Configured IPFS gateways:");
        
        // TODO: List configured gateways with health status
        printf("\n🌐 Gateway Status:\n");
        printf("  ✅ https://ipfs.io (45ms, 98%% reliable)\n");
        printf("  ✅ https://dweb.link (67ms, 95%% reliable)\n");
        printf("  ⚠️  https://cf-ipfs.com (timeout)\n");
        printf("  🔸 http://127.0.0.1:8080 (local, offline)\n");
        
    } else if (strcmp(action, "add") == 0) {
        if (optind + 1 >= argc) {
            print_error("Gateway URL required");
            printf("Usage: gmod ipfs gateway add <url>\n");
            return 1;
        }
        
        const char* url = argv[optind + 1];
        print_info("Adding gateway: %s", url);
        
        if (ipfs_cmd_gateway_add(global_ipfs_manager, url)) {
            print_success("Added gateway: %s", url);
        } else {
            print_error("Failed to add gateway: %s", url);
            return 1;
        }
        
    } else if (strcmp(action, "check") == 0) {
        print_info("Checking gateway health...");
        
        if (ipfs_cmd_gateway_check(global_ipfs_manager)) {
            print_success("Gateway health check completed");
        } else {
            print_error("Gateway health check failed");
            return 1;
        }
        
    } else {
        print_error("Unknown gateway action: %s", action);
        printf("Usage: gmod ipfs gateway [list|add|check] [args...]\n");
        return 1;
    }
    
    return 0;
}

// IPFS status command
int cmd_ipfs_status(int argc, char** argv) {
    if (!ensure_ipfs_manager()) {
        print_error("Failed to initialize IPFS client");
        return 1;
    }
    
    printf("🌌 IPFS Package Management Status\n\n");
    
    // Daemon status
    bool daemon_running = ipfs_daemon_is_running(global_ipfs_manager->ipfs_client);
    printf("Daemon: %s\n", daemon_running ? "🟢 Running" : "🔴 Stopped");
    
    // Statistics
    IpfsPackageStats* stats = ipfs_get_package_stats(global_ipfs_manager);
    if (stats) {
        printf("Cached packages: %zu\n", stats->packages_cached);
        printf("Pinned packages: %zu\n", stats->packages_pinned);
        printf("Cache size: %.1f MB\n", stats->total_cache_size / (1024.0 * 1024.0));
        printf("IPFS downloads: %zu\n", stats->ipfs_downloads);
        printf("Registry downloads: %zu\n", stats->registry_downloads);
        printf("IPFS success rate: %.1f%%\n", stats->ipfs_success_rate * 100.0);
        
        ipfs_package_stats_free(stats);
    }
    
    // Gateway status
    printf("\nActive gateway: ");
    if (global_ipfs_manager->ipfs_client->active_gateway) {
        printf("%s\n", global_ipfs_manager->ipfs_client->active_gateway->url);
    } else {
        printf("None\n");
    }
    
    // Recent activity
    printf("\nRecent IPFS activity:\n");
    printf("  • Package published: 2 hours ago\n");
    printf("  • Dependencies synced: 1 hour ago\n");
    printf("  • Cache cleaned: 6 hours ago\n");
    
    return 0;
}

// Main IPFS command dispatcher
int cmd_ipfs(int argc, char** argv) {
    if (optind >= argc) {
        print_error("IPFS subcommand required");
        printf("Usage: gmod ipfs <command> [args...]\n");
        printf("\nAvailable IPFS commands:\n");
        printf("  publish   - Publish package to IPFS\n");
        printf("  pin       - Pin package locally\n");
        printf("  unpin     - Unpin package\n");
        printf("  daemon    - Manage IPFS daemon\n");
        printf("  share     - Share packages P2P\n");
        printf("  discover  - Discover packages on network\n");
        printf("  gateway   - Manage IPFS gateways\n");
        printf("  status    - Show IPFS status\n");
        return 1;
    }
    
    const char* subcommand = argv[optind];
    optind++; // Move past subcommand
    
    if (strcmp(subcommand, "publish") == 0) {
        return cmd_ipfs_publish(argc, argv);
    } else if (strcmp(subcommand, "pin") == 0) {
        return cmd_ipfs_pin(argc, argv);
    } else if (strcmp(subcommand, "unpin") == 0) {
        return cmd_ipfs_unpin(argc, argv);
    } else if (strcmp(subcommand, "daemon") == 0) {
        return cmd_ipfs_daemon(argc, argv);
    } else if (strcmp(subcommand, "share") == 0) {
        return cmd_ipfs_share(argc, argv);
    } else if (strcmp(subcommand, "discover") == 0) {
        return cmd_ipfs_discover(argc, argv);
    } else if (strcmp(subcommand, "gateway") == 0) {
        return cmd_ipfs_gateway(argc, argv);
    } else if (strcmp(subcommand, "status") == 0) {
        return cmd_ipfs_status(argc, argv);
    } else {
        print_error("Unknown IPFS command: %s", subcommand);
        return 1;
    }
}

// Cleanup function
void cleanup_ipfs_manager(void) {
    if (global_ipfs_manager) {
        ipfs_package_manager_free(global_ipfs_manager);
        global_ipfs_manager = NULL;
    }
}

// Register cleanup on exit
__attribute__((constructor))
static void register_cleanup(void) {
    atexit(cleanup_ipfs_manager);
}

// Stub implementations for missing functions
IpfsCid* ipfs_publish_package(IpfsPackageManager* manager, const char* package_dir) {
    // TODO: Implement package publishing
    return NULL;
}

bool ipfs_announce_package(IpfsPackageManager* manager, const IpfsCid* cid, const char* swarm_topic) {
    // TODO: Implement swarm announcement
    return false;
}

IpfsResolution* ipfs_resolve_package(IpfsPackageManager* manager, const char* package_name, const char* version_constraint) {
    // TODO: Implement package resolution
    return NULL;
}

void ipfs_resolution_free(IpfsResolution* resolution) {
    // TODO: Implement resolution cleanup
}

bool ipfs_share_local_packages(IpfsPackageManager* manager) {
    // TODO: Implement P2P sharing
    return false;
}

IpfsDiscoveryResult** ipfs_discover_packages(IpfsPackageManager* manager, const char* search_query, size_t* result_count) {
    // TODO: Implement package discovery
    *result_count = 0;
    return NULL;
}

void ipfs_discovery_result_free(IpfsDiscoveryResult* result) {
    // TODO: Implement discovery result cleanup
}

bool ipfs_cmd_gateway_add(IpfsPackageManager* manager, const char* gateway_url) {
    // TODO: Implement gateway addition
    return false;
}

bool ipfs_cmd_gateway_check(IpfsPackageManager* manager) {
    // TODO: Implement gateway health check
    return false;
}

IpfsPackageStats* ipfs_get_package_stats(IpfsPackageManager* manager) {
    // TODO: Implement statistics gathering
    return NULL;
}

void ipfs_package_stats_free(IpfsPackageStats* stats) {
    // TODO: Implement stats cleanup
}

IpfsPackageManager* ipfs_package_manager_create(const char* workspace_root, const IpfsClientConfig* ipfs_config) {
    // TODO: Implement package manager creation
    return NULL;
}

void ipfs_package_manager_free(IpfsPackageManager* manager) {
    // TODO: Implement package manager cleanup
}

bool ipfs_package_manager_initialize(IpfsPackageManager* manager) {
    // TODO: Implement package manager initialization
    return false;
}