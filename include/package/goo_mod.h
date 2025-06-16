#ifndef GOO_MOD_H
#define GOO_MOD_H

#include "module.h"
#include <stdbool.h>
#include <stddef.h>

// Forward declarations
typedef struct GooMod GooMod;
typedef struct ComptimeDeps ComptimeDeps;
typedef struct FeatureConfig FeatureConfig;
typedef struct SecurityConfig SecurityConfig;
typedef struct IntelligenceConfig IntelligenceConfig;
typedef struct ResolveResult ResolveResult;
typedef struct DependencyGraph DependencyGraph;
typedef struct ResolvedDependency ResolvedDependency;

// Feature configuration
typedef struct FeatureConfig {
    char* name;
    bool default_enabled;
    char** requires;          // Required features
    size_t requires_count;
    char** conflicts;         // Conflicting features
    size_t conflicts_count;
    char* description;
    struct FeatureConfig* next;
} FeatureConfig;

// Compile-time dependency resolution
typedef struct ComptimeDeps {
    bool enabled;
    bool verify_compatibility;
    bool security_scan;
    bool performance_analysis;
    char* resolution_strategy;    // "latest_compatible", "minimal", "optimal"
    char** optimization_criteria;
    size_t criteria_count;
} ComptimeDeps;

// Security configuration  
typedef struct SecurityConfig {
    bool signature_verification;
    bool reproducible_builds;
    char* supply_chain_analysis;  // "none", "basic", "deep"
    bool sandbox_analysis;
    char** vulnerability_sources;
    size_t source_count;
    char* severity_threshold;     // "low", "medium", "high", "critical"
    bool auto_update;
} SecurityConfig;

// AI Intelligence configuration
typedef struct IntelligenceConfig {
    bool optimize_dependencies;
    bool suggest_alternatives;
    bool predict_compatibility;
    char* security_analysis;      // "none", "basic", "strict"
    bool performance_analysis;
    bool predictive_caching;
    bool semantic_caching;
} IntelligenceConfig;

// Registry configuration
typedef struct RegistryConfig {
    char* primary;
    char** mirrors;
    size_t mirror_count;
    char** corporate;
    size_t corporate_count;
    char** distributed;          // IPFS, blockchain, etc.
    size_t distributed_count;
} RegistryConfig;

// Caching configuration
typedef struct CachingConfig {
    bool predictive_caching;
    bool semantic_caching;
    char* cache_network;         // "local", "regional", "global"
    bool offline_mode;
    bool predictive_download;
    bool local_mirrors;
    bool p2p_sharing;
} CachingConfig;

// Enhanced Goo module definition
typedef struct GooMod {
    // Basic module info
    char* module_path;           // "github.com/user/project"
    Version* version;
    char* goo_version;           // Minimum Goo version
    char* description;
    char* license;
    char** authors;
    size_t author_count;
    char** keywords;
    size_t keyword_count;
    
    // Dependencies
    Dependency* dependencies;
    Dependency* dev_dependencies;
    Dependency* build_dependencies;
    
    // Features
    FeatureConfig* features;
    char** default_features;
    size_t default_feature_count;
    
    // Build configuration
    char* build_script;
    char** source_dirs;
    size_t source_dir_count;
    char** exclude_patterns;
    size_t exclude_pattern_count;
    char* main_file;
    
    // Advanced configurations
    ComptimeDeps* comptime;
    SecurityConfig* security;
    IntelligenceConfig* intelligence;
    RegistryConfig* registries;
    CachingConfig* caching;
    
    // Workspace configuration
    bool is_workspace;
    char** workspace_members;
    size_t workspace_member_count;
    
    // Metadata
    time_t created_at;
    time_t updated_at;
    char* checksum;
} GooMod;

// Dependency resolution context
typedef struct ResolutionContext {
    GooMod* root_module;
    char** enabled_features;
    size_t feature_count;
    bool dev_mode;
    bool offline_mode;
    char* target_platform;
    char* optimization_level;
} ResolutionContext;

// Resolution criteria for multi-objective optimization
typedef struct ResolutionCriteria {
    float compatibility_weight;   // Must be compatible (weight 1.0)
    float security_weight;        // Security priority (0.9)
    float performance_weight;     // Performance preference (0.7)
    float binary_size_weight;     // Binary size preference (0.6)
    float maintenance_weight;     // Maintenance quality (0.5)
    float popularity_weight;      // Community adoption (0.3)
    bool ai_scoring;
    bool pareto_optimization;
} ResolutionCriteria;

// Function declarations

// GooMod lifecycle
GooMod* goo_mod_create(const char* module_path, const char* version);
void goo_mod_free(GooMod* gmod);
GooMod* goo_mod_parse_file(const char* filepath);
GooMod* goo_mod_parse_string(const char* content);
bool goo_mod_save_file(const GooMod* gmod, const char* filepath);
char* goo_mod_to_string(const GooMod* gmod);

// Feature management
FeatureConfig* feature_config_create(const char* name, bool default_enabled);
void feature_config_free(FeatureConfig* feature);
bool feature_add_requirement(FeatureConfig* feature, const char* required_feature);
bool feature_add_conflict(FeatureConfig* feature, const char* conflicting_feature);
FeatureConfig* goo_mod_add_feature(GooMod* gmod, const char* name, bool default_enabled);
bool goo_mod_enable_feature(GooMod* gmod, const char* feature_name);
bool goo_mod_disable_feature(GooMod* gmod, const char* feature_name);
char** goo_mod_get_active_features(const GooMod* gmod, const ResolutionContext* context, size_t* count);

// Compile-time configuration
ComptimeDeps* comptime_deps_create(void);
void comptime_deps_free(ComptimeDeps* comptime);
bool comptime_deps_add_criteria(ComptimeDeps* comptime, const char* criteria);

// Security configuration
SecurityConfig* security_config_create(void);
void security_config_free(SecurityConfig* security);
bool security_config_add_source(SecurityConfig* security, const char* source);

// Intelligence configuration
IntelligenceConfig* intelligence_config_create(void);
void intelligence_config_free(IntelligenceConfig* intelligence);

// Registry configuration
RegistryConfig* registry_config_create(void);
void registry_config_free(RegistryConfig* registry);
bool registry_config_add_mirror(RegistryConfig* registry, const char* mirror_url);

// Caching configuration
CachingConfig* caching_config_create(void);
void caching_config_free(CachingConfig* caching);

// Resolution result
typedef struct ResolveResult {
    bool success;
    char* error_message;
    int dependency_count;
    char** warnings;
    size_t warning_count;
} ResolveResult;

// Dependency graph for analysis
typedef struct DependencyGraph {
    Dependency** dependencies;
    size_t dependency_count;
    size_t capacity;
    char* root_package;
    bool has_cycles;
    char** cycle_info;
    size_t cycle_count;
} DependencyGraph;

// Resolved dependency with metadata
typedef struct ResolvedDependency {
    char* name;
    char* version;
    char* source_url;
    char* checksum;
    size_t download_size;
    bool is_dev_dependency;
    float security_score;
    float performance_score;
    char** features;
    size_t feature_count;
} ResolvedDependency;

// Dependency resolution with AI enhancement
typedef struct DependencyResolver DependencyResolver;

DependencyResolver* dependency_resolver_create(const ResolutionCriteria* criteria);
void dependency_resolver_free(DependencyResolver* resolver);
ResolveResult dependency_resolver_resolve(DependencyResolver* resolver, 
                                        const GooMod* gmod, 
                                        const ResolutionContext* context,
                                        DependencyGraph** result);

// Multi-criteria optimization
float calculate_dependency_score(const ResolvedDependency* dep, 
                               const ResolutionCriteria* criteria);
bool pareto_optimize_dependencies(DependencyGraph* graph, 
                                const ResolutionCriteria* criteria);

// Compile-time analysis
bool analyze_compatibility(const DependencyGraph* graph, char** report);
bool analyze_security_vulnerabilities(const DependencyGraph* graph, char** report);
bool analyze_performance_impact(const DependencyGraph* graph, char** report);
float estimate_binary_size_impact(const DependencyGraph* graph);

// Smart dependency suggestions
typedef struct DependencySuggestion {
    char* package_name;
    char* version_constraint;
    char* reason;               // Why this is suggested
    float confidence_score;     // AI confidence (0.0-1.0)
    char** similar_packages;    // Alternative packages
    size_t similar_count;
} DependencySuggestion;

DependencySuggestion** suggest_dependencies(const GooMod* gmod, 
                                          const char* query, 
                                          size_t* suggestion_count);
void dependency_suggestion_free(DependencySuggestion* suggestion);

// Workspace management
bool goo_mod_init_workspace(const char* path, const char* name);
bool goo_mod_add_workspace_member(GooMod* gmod, const char* member_path);
bool goo_mod_remove_workspace_member(GooMod* gmod, const char* member_path);
GooMod** goo_mod_load_workspace_members(const GooMod* gmod, size_t* count);

// Version strategy management
typedef enum {
    VERSION_STRATEGY_SEMANTIC,     // Traditional semver
    VERSION_STRATEGY_ADAPTIVE,     // AI-enhanced compatibility
    VERSION_STRATEGY_STRICT,       // Exact versions only
    VERSION_STRATEGY_LATEST        // Always use latest
} VersionStrategy;

bool set_version_strategy(GooMod* gmod, VersionStrategy strategy);
VersionStrategy get_version_strategy(const GooMod* gmod);

// Build integration
typedef struct BuildContext {
    char* target_arch;
    char* target_os;
    char* optimization_level;
    bool debug_symbols;
    char** feature_flags;
    size_t feature_count;
    char* output_dir;
} BuildContext;

bool goo_mod_prepare_build(const GooMod* gmod, 
                          const BuildContext* build_ctx,
                          char*** include_paths, 
                          char*** library_paths,
                          size_t* path_count);

// Validation and linting
typedef struct ValidationResult {
    bool is_valid;
    char** errors;
    size_t error_count;
    char** warnings;
    size_t warning_count;
    char** suggestions;
    size_t suggestion_count;
} ValidationResult;

ValidationResult* goo_mod_validate(const GooMod* gmod);
void validation_result_free(ValidationResult* result);

// Security and integrity
bool goo_mod_verify_integrity(const GooMod* gmod);
bool goo_mod_scan_vulnerabilities(const GooMod* gmod, char** report);
char* goo_mod_calculate_checksum(const GooMod* gmod);

// Import analysis
typedef struct ImportAnalysis {
    char** direct_imports;      // Direct package imports
    size_t direct_count;
    char** transitive_imports;  // Transitive dependencies
    size_t transitive_count;
    char** unused_dependencies; // Declared but unused
    size_t unused_count;
    char** missing_dependencies; // Used but not declared
    size_t missing_count;
} ImportAnalysis;

ImportAnalysis* analyze_imports(const GooMod* gmod, const char* source_dir);
void import_analysis_free(ImportAnalysis* analysis);

// Default configurations
GooMod* goo_mod_create_default(const char* module_path);
ResolutionCriteria* resolution_criteria_default(void);
ResolutionContext* resolution_context_default(void);

#endif // GOO_MOD_H