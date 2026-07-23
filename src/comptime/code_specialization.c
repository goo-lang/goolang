#include "code_specialization.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Hash function for specialization keys
static uint64_t hash_string(const char* str) {
    uint64_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static uint64_t hash_param(const SpecializationParam* param) {
    uint64_t hash = hash_string(param->param_name);
    hash ^= (uint64_t)param->type << 32;
    
    switch (param->type) {
        case SPEC_TYPE_CONSTANT:
            hash ^= (uint64_t)param->value.constant_value;
            break;
        case SPEC_TYPE_TYPE:
            hash ^= (uint64_t)(uintptr_t)param->value.type_value;
            break;
        case SPEC_TYPE_RANGE:
            hash ^= (uint64_t)param->value.range_value.min;
            hash ^= (uint64_t)param->value.range_value.max << 16;
            break;
        case SPEC_TYPE_PATTERN:
            hash ^= hash_string(param->value.pattern_value);
            break;
        default:
            break;
    }
    
    return hash;
}

// Context management
SpecializationContext* specialization_context_create(void) {
    SpecializationContext* context = xcalloc(1, sizeof(SpecializationContext));
    if (!context) return NULL;
    
    // Create cache with default size
    context->cache = specialization_cache_create(1024);
    if (!context->cache) {
        free(context);
        return NULL;
    }
    
    // Initialize heuristics with defaults
    context->heuristics = xcalloc(1, sizeof(SpecializationHeuristics));
    if (!context->heuristics) {
        specialization_cache_free(context->cache);
        free(context);
        return NULL;
    }
    
    // Default heuristics
    context->heuristics->min_benefit_threshold = 0.2f;
    context->heuristics->min_call_count = 100;
    context->heuristics->max_compilation_time = 0.1; // 100ms
    context->heuristics->max_code_size_increase = 10 * 1024; // 10KB
    
    context->heuristics->enable_aggressive_inlining = true;
    context->heuristics->enable_constant_propagation = true;
    context->heuristics->enable_range_analysis = true;
    context->heuristics->enable_type_specialization = true;
    context->heuristics->enable_pattern_matching = true;
    context->heuristics->enable_hardware_specific = true;
    
    context->heuristics->max_specializations_per_function = 10;
    context->heuristics->max_inline_depth = 5;
    context->heuristics->max_unroll_factor = 8;
    
    return context;
}

void specialization_context_free(SpecializationContext* context) {
    if (!context) return;
    
    specialization_cache_free(context->cache);
    free(context->heuristics);
    
    // Free profile data
    free(context->profile_data.call_counts);
    free(context->profile_data.execution_times);
    free(context->profile_data.type_profiles);
    free(context->profile_data.value_profiles);
    
    // Don't free active specializations here - they're owned by the cache
    // and will be freed when the cache is freed
    
    free(context);
}

bool specialization_context_initialize(SpecializationContext* context) {
    if (!context) return false;
    
    // Initialize profile data arrays (simplified - would be dynamic in practice)
    size_t initial_size = 1024;
    context->profile_data.call_counts = calloc(initial_size, sizeof(uint64_t));
    context->profile_data.execution_times = calloc(initial_size, sizeof(double));
    context->profile_data.type_profiles = calloc(initial_size, sizeof(Type*));
    context->profile_data.value_profiles = calloc(initial_size, sizeof(int64_t*));
    
    return true;
}

// Specialization analysis
bool should_specialize_function(SpecializationContext* context,
                               const char* function_name,
                               const SpecializationParam* params,
                               size_t param_count) {
    if (!context || !function_name || !params) return false;
    
    // Check if we've already specialized this combination
    SpecializationKey* key = create_specialization_key(function_name, params, param_count);
    if (!key) return false;
    
    SpecializedFunction* existing = cache_lookup(context->cache, key);
    free_specialization_key(key);
    
    if (existing) {
        return false; // Already specialized
    }
    
    // Check heuristics
    // In a real implementation, we'd look up the actual call count
    // For now, we'll use a placeholder
    size_t call_count = 1000; // Placeholder
    
    if (call_count < context->heuristics->min_call_count) {
        return false;
    }
    
    // Estimate benefit
    float benefit = 0.0f;
    for (size_t i = 0; i < param_count; i++) {
        benefit += params[i].specialization_benefit;
    }
    benefit /= param_count;
    
    return benefit >= context->heuristics->min_benefit_threshold;
}

float estimate_specialization_benefit(SpecializationContext* context,
                                    ASTNode* function,
                                    const SpecializationParam* params,
                                    size_t param_count) {
    if (!context || !params || param_count == 0) return 0.0f;
    
    float total_benefit = 0.0f;
    
    for (size_t i = 0; i < param_count; i++) {
        float param_benefit = 0.0f;
        
        switch (params[i].type) {
            case SPEC_TYPE_CONSTANT:
                // Constant propagation can eliminate branches and enable folding
                param_benefit = 0.3f;
                break;
                
            case SPEC_TYPE_TYPE:
                // Type specialization enables devirtualization and inlining
                param_benefit = 0.4f;
                break;
                
            case SPEC_TYPE_RANGE:
                // Range analysis can eliminate bounds checks
                param_benefit = 0.25f;
                break;
                
            case SPEC_TYPE_PATTERN:
                // Pattern matching can use specialized algorithms
                param_benefit = 0.35f;
                break;
                
            case SPEC_TYPE_HARDWARE:
                // Hardware specialization can use SIMD/GPU
                param_benefit = 0.5f;
                break;
                
            case SPEC_TYPE_HOTPATH:
                // Hot path optimization has high impact
                param_benefit = 0.6f;
                break;
                
            default:
                param_benefit = 0.1f;
                break;
        }
        
        total_benefit += param_benefit * params[i].specialization_benefit;
    }
    
    // Adjust based on function characteristics
    // (In practice, we'd analyze the AST)
    
    return total_benefit / param_count;
}

// Specialization generation
SpecializedFunction* generate_specialization(SpecializationContext* context,
                                           ASTNode* function,
                                           const SpecializationParam* params,
                                           size_t param_count) {
    if (!context || !function || !params) return NULL;
    
    // Create specialized function
    SpecializedFunction* specialized = xcalloc(1, sizeof(SpecializedFunction));
    if (!specialized) return NULL;
    
    // Create key
    const char* function_name = "unknown"; // Would extract from AST
    specialized->key = create_specialization_key(function_name, params, param_count);
    if (!specialized->key) {
        free(specialized);
        return NULL;
    }
    
    // Record start time
    clock_t start = clock();
    
    // Clone the original AST
    specialized->original_function = function;
    specialized->specialized_ast = function; // Would deep clone in practice
    
    // Apply specializations
    bool success = true;
    
    // 1. Constant propagation
    if (context->heuristics->enable_constant_propagation) {
        success &= propagate_constants(specialized->specialized_ast, params, param_count);
        success &= fold_constant_expressions(specialized->specialized_ast);
        success &= eliminate_dead_code(specialized->specialized_ast);
    }
    
    // 2. Type specialization
    if (context->heuristics->enable_type_specialization) {
        // Extract types from params. The original used a VLA
        // `Type* types[param_count]`; CompCert doesn't support VLAs.
        // Heap allocation works for both compilers and is freed
        // below; param_count is small (function arity) so the
        // allocation overhead is negligible.
        Type** types = (Type**)malloc(sizeof(Type*) * (param_count > 0 ? param_count : 1));
        size_t type_count = 0;
        for (size_t i = 0; i < param_count; i++) {
            if (params[i].type == SPEC_TYPE_TYPE) {
                types[type_count++] = params[i].value.type_value;
            }
        }
        if (type_count > 0) {
            success &= specialize_for_types(specialized->specialized_ast,
                                          (const Type**)types, type_count);
        }
        free(types);
    }
    
    // 3. Range analysis
    if (context->heuristics->enable_range_analysis) {
        success &= specialize_for_ranges(specialized->specialized_ast, params, param_count);
        success &= comptime_eliminate_bounds_checks(specialized->specialized_ast, params);
    }
    
    // 4. Hardware specialization
    if (context->heuristics->enable_hardware_specific && context->hardware_context) {
        success &= specialize_for_hardware(specialized->specialized_ast, 
                                         context->hardware_context->profile);
    }
    
    // Record specialization time
    clock_t end = clock();
    specialized->specialization_time = (double)(end - start) / CLOCKS_PER_SEC;
    
    if (!success) {
        free_specialization_key(specialized->key);
        free(specialized);
        return NULL;
    }
    
    // Calculate benefit score
    specialized->benefit_score = estimate_specialization_benefit(context, function, 
                                                               params, param_count);
    specialized->compilation_cost = calculate_compilation_cost(100, params); // Placeholder size
    
    // Initialize other fields
    specialized->created_at = time(NULL);
    specialized->last_used = specialized->created_at;
    
    // Add to cache
    cache_insert(context->cache, specialized);
    
    // Add to active list
    specialized->next = context->active_specializations;
    context->active_specializations = specialized;
    context->active_count++;
    
    // Update statistics
    context->total_specializations++;
    context->total_specialization_time += specialized->specialization_time;
    context->total_benefit += specialized->benefit_score;
    
    return specialized;
}

// Constant propagation implementation
bool propagate_constants(ASTNode* ast, const SpecializationParam* params, size_t param_count) {
    if (!ast || !params) return false;
    
    // This is a simplified implementation
    // In practice, we'd walk the AST and replace parameter references
    // with constant values where applicable
    
    // For demonstration, we'll just mark success
    return true;
}

bool fold_constant_expressions(ASTNode* ast) {
    if (!ast) return false;
    
    // Walk the AST and fold constant expressions
    // For example: 2 + 3 -> 5
    
    return true;
}

bool eliminate_dead_code(ASTNode* ast) {
    if (!ast) return false;
    
    // Remove unreachable code
    // For example: if (false) { ... } -> remove block
    
    return true;
}

// Type specialization
bool specialize_for_types(ASTNode* ast, const Type** param_types, size_t param_count) {
    if (!ast || !param_types) return false;
    
    // Replace generic operations with type-specific ones
    // Enable devirtualization of method calls
    
    return true;
}

// Range analysis
bool specialize_for_ranges(ASTNode* ast, const SpecializationParam* params, size_t param_count) {
    if (!ast || !params) return false;
    
    // Use range information to optimize
    // For example: if we know i is in [0, 10], we can unroll small loops
    
    return true;
}

bool comptime_eliminate_bounds_checks(ASTNode* ast, const SpecializationParam* params) {
    if (!ast || !params) return false;
    
    // Remove array bounds checks when we can prove they're safe
    
    return true;
}

// Hardware specialization
bool specialize_for_hardware(ASTNode* ast, HardwareProfile* profile) {
    if (!ast || !profile) return false;
    
    // Apply hardware-specific optimizations
    if (profile->capabilities & HW_CAP_SIMD_AVX2) {
        auto_vectorize_loops(ast, 256);
    } else if (profile->capabilities & HW_CAP_SIMD_NEON) {
        auto_vectorize_loops(ast, 128);
    }
    
    return true;
}

bool auto_vectorize_loops(ASTNode* ast, int vector_width) {
    if (!ast) return false;
    
    // Find vectorizable loops and transform them
    // This would analyze data dependencies and generate SIMD operations
    
    return true;
}

// Cache management
SpecializationCache* specialization_cache_create(size_t capacity) {
    SpecializationCache* cache = xcalloc(1, sizeof(SpecializationCache));
    if (!cache) return NULL;
    
    cache->bucket_count = capacity;
    cache->buckets = calloc(capacity, sizeof(SpecializedFunction*));
    if (!cache->buckets) {
        free(cache);
        return NULL;
    }
    
    cache->max_entries = capacity * 4; // Allow 4x load factor
    cache->eviction_policy = EVICT_LRU;
    
    return cache;
}

void specialization_cache_free(SpecializationCache* cache) {
    if (!cache) return;
    
    // Free all cached specializations
    for (size_t i = 0; i < cache->bucket_count; i++) {
        SpecializedFunction* spec = cache->buckets[i];
        while (spec) {
            SpecializedFunction* next = spec->next;
            free_specialization_key(spec->key);
            // Note: We don't free the AST nodes as they may be shared
            free(spec);
            spec = next;
        }
    }
    
    free(cache->buckets);
    free(cache);
}

SpecializedFunction* cache_lookup(SpecializationCache* cache, const SpecializationKey* key) {
    if (!cache || !key) return NULL;
    
    size_t bucket = key->hash % cache->bucket_count;
    SpecializedFunction* spec = cache->buckets[bucket];
    
    while (spec) {
        if (specialization_keys_equal(spec->key, key)) {
            cache->hits++;
            spec->last_used = time(NULL);
            spec->call_count++;
            return spec;
        }
        spec = spec->next;
    }
    
    cache->misses++;
    return NULL;
}

bool cache_insert(SpecializationCache* cache, SpecializedFunction* specialized) {
    if (!cache || !specialized || !specialized->key) return false;
    
    // Check if we need to evict
    if (cache->total_entries >= cache->max_entries) {
        cache_evict_if_needed(cache);
    }
    
    size_t bucket = specialized->key->hash % cache->bucket_count;
    specialized->next = cache->buckets[bucket];
    cache->buckets[bucket] = specialized;
    cache->total_entries++;
    
    return true;
}

bool cache_evict_if_needed(SpecializationCache* cache) {
    if (!cache || cache->total_entries < cache->max_entries) {
        return false;
    }
    
    // Find least recently used entry
    SpecializedFunction* lru = NULL;
    SpecializedFunction** lru_prev = NULL;
    time_t oldest_time = time(NULL);
    
    for (size_t i = 0; i < cache->bucket_count; i++) {
        SpecializedFunction** prev = &cache->buckets[i];
        SpecializedFunction* spec = cache->buckets[i];
        
        while (spec) {
            if (spec->last_used < oldest_time) {
                oldest_time = spec->last_used;
                lru = spec;
                lru_prev = prev;
            }
            prev = &spec->next;
            spec = spec->next;
        }
    }
    
    if (lru && lru_prev) {
        *lru_prev = lru->next;
        free_specialization_key(lru->key);
        free(lru);
        cache->total_entries--;
        cache->evictions++;
        return true;
    }
    
    return false;
}

// Key management
SpecializationKey* create_specialization_key(const char* function_name,
                                           const SpecializationParam* params,
                                           size_t param_count) {
    if (!function_name || !params) return NULL;
    
    SpecializationKey* key = xcalloc(1, sizeof(SpecializationKey));
    if (!key) return NULL;
    
    key->function_name = strdup(function_name);
    if (!key->function_name) {
        free(key);
        return NULL;
    }
    
    // Copy parameters
    key->params = calloc(param_count, sizeof(SpecializationParam));
    if (!key->params) {
        free(key->function_name);
        free(key);
        return NULL;
    }
    
    key->param_count = param_count;
    
    // Deep copy parameters
    for (size_t i = 0; i < param_count; i++) {
        key->params[i] = params[i];
        key->params[i].param_name = strdup(params[i].param_name);
        
        if (params[i].type == SPEC_TYPE_PATTERN && params[i].value.pattern_value) {
            key->params[i].value.pattern_value = strdup(params[i].value.pattern_value);
        }
    }
    
    // Calculate hash
    key->hash = hash_specialization_key(key);
    
    return key;
}

void free_specialization_key(SpecializationKey* key) {
    if (!key) return;
    
    free(key->function_name);
    
    for (size_t i = 0; i < key->param_count; i++) {
        free(key->params[i].param_name);
        if (key->params[i].type == SPEC_TYPE_PATTERN) {
            free(key->params[i].value.pattern_value);
        }
    }
    
    free(key->params);
    free(key);
}

uint64_t hash_specialization_key(const SpecializationKey* key) {
    if (!key) return 0;
    
    uint64_t hash = hash_string(key->function_name);
    
    for (size_t i = 0; i < key->param_count; i++) {
        hash ^= hash_param(&key->params[i]) << (i % 8);
    }
    
    return hash;
}

bool specialization_keys_equal(const SpecializationKey* a, const SpecializationKey* b) {
    if (!a || !b) return false;
    if (a == b) return true;
    
    if (strcmp(a->function_name, b->function_name) != 0) return false;
    if (a->param_count != b->param_count) return false;
    
    for (size_t i = 0; i < a->param_count; i++) {
        if (strcmp(a->params[i].param_name, b->params[i].param_name) != 0) return false;
        if (a->params[i].type != b->params[i].type) return false;
        
        // Compare values based on type
        switch (a->params[i].type) {
            case SPEC_TYPE_CONSTANT:
                if (a->params[i].value.constant_value != b->params[i].value.constant_value) {
                    return false;
                }
                break;
            case SPEC_TYPE_RANGE:
                if (a->params[i].value.range_value.min != b->params[i].value.range_value.min ||
                    a->params[i].value.range_value.max != b->params[i].value.range_value.max) {
                    return false;
                }
                break;
            // ... other cases
            default:
                break;
        }
    }
    
    return true;
}

// Cost-benefit analysis
float calculate_benefit_score(const SpecializedFunction* specialized) {
    if (!specialized) return 0.0f;
    
    // Consider multiple factors
    float performance_gain = 1.0f; // Would measure actual speedup
    float call_frequency = (float)specialized->call_count / 1000.0f;
    float code_efficiency = 1.0f / (1.0f + specialized->code_size / 1024.0f);
    
    return (performance_gain * 0.5f + call_frequency * 0.3f + code_efficiency * 0.2f);
}

float calculate_compilation_cost(size_t ast_size, const SpecializationParam* params) {
    if (!params) return 1.0f;
    
    // Estimate based on AST size and specialization complexity
    float base_cost = (float)ast_size / 1000.0f;
    float specialization_cost = 0.0f;
    
    // Add cost for each specialization type
    // (More complex specializations have higher cost)
    
    return base_cost + specialization_cost;
}

// Compile-time intrinsics
ComptimeValue* comptime_specialize_for(ComptimeValue* function, ComptimeValue* params) {
    if (!function || !params) return NULL;
    
    // This would trigger specialization at compile time
    ComptimeValue* result = xmalloc(sizeof(ComptimeValue));
    if (!result) return NULL;
    
    result->type = COMPTIME_VALUE_FUNCTION;
    result->function_value.function_node = function->function_value.function_node;
    
    return result;
}

ComptimeValue* comptime_force_inline(ComptimeValue* function) {
    if (!function || function->type != COMPTIME_VALUE_FUNCTION) return NULL;
    
    // Mark function for aggressive inlining
    // This would set metadata on the AST node
    
    return function;
}

// Debug and statistics
void print_specialization_stats(const SpecializationContext* context) {
    if (!context) return;
    
    printf("Specialization Statistics:\n");
    printf("  Total specializations: %zu\n", context->total_specializations);
    printf("  Active specializations: %zu\n", context->active_count);
    printf("  Total specialization time: %.3f seconds\n", context->total_specialization_time);
    printf("  Average benefit score: %.2f\n", 
           context->total_specializations > 0 ? 
           context->total_benefit / context->total_specializations : 0.0);
    
    printf("  Cache statistics:\n");
    printf("    Hits: %zu\n", context->cache->hits);
    printf("    Misses: %zu\n", context->cache->misses);
    printf("    Hit rate: %.2f%%\n", 
           context->cache->hits + context->cache->misses > 0 ?
           100.0 * context->cache->hits / (context->cache->hits + context->cache->misses) : 0.0);
    printf("    Evictions: %zu\n", context->cache->evictions);
}

void print_specialized_function(const SpecializedFunction* specialized) {
    if (!specialized || !specialized->key) return;
    
    printf("Specialized Function: %s\n", specialized->key->function_name);
    printf("  Parameters:\n");
    
    for (size_t i = 0; i < specialized->key->param_count; i++) {
        const SpecializationParam* param = &specialized->key->params[i];
        printf("    %s: ", param->param_name);
        
        switch (param->type) {
            case SPEC_TYPE_CONSTANT:
                printf("constant = %lld", (long long)param->value.constant_value);
                break;
            case SPEC_TYPE_RANGE:
                printf("range = [%lld, %lld]", 
                       (long long)param->value.range_value.min,
                       (long long)param->value.range_value.max);
                break;
            case SPEC_TYPE_PATTERN:
                printf("pattern = %s", param->value.pattern_value);
                break;
            default:
                printf("type = %d", param->type);
                break;
        }
        printf(" (benefit: %.2f)\n", param->specialization_benefit);
    }
    
    printf("  Performance:\n");
    printf("    Specialization time: %.3f ms\n", specialized->specialization_time * 1000);
    printf("    Call count: %zu\n", specialized->call_count);
    printf("    Code size: %zu bytes\n", specialized->code_size);
    printf("    Benefit score: %.2f\n", specialized->benefit_score);
    
    printf("  Optimizations:\n");
    printf("    Inlined: %s\n", specialized->is_inlined ? "yes" : "no");
    printf("    Vectorized: %s\n", specialized->is_vectorized ? "yes" : "no");
    if (specialized->is_unrolled) {
        printf("    Unrolled: %dx\n", specialized->unroll_factor);
    }
    printf("    Hardware intrinsics: %s\n", 
           specialized->uses_hardware_intrinsics ? "yes" : "no");
}