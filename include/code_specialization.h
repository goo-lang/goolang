#ifndef CODE_SPECIALIZATION_H
#define CODE_SPECIALIZATION_H

#include "ast.h"
#include "types.h"
#include "comptime.h"
#include "hardware_aware.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Forward declarations
typedef struct SpecializationContext SpecializationContext;
typedef struct SpecializedFunction SpecializedFunction;
typedef struct SpecializationKey SpecializationKey;
typedef struct SpecializationCache SpecializationCache;

// Specialization types
typedef enum {
    SPEC_TYPE_CONSTANT,          // Specialization for constant values
    SPEC_TYPE_TYPE,              // Type-specific specialization
    SPEC_TYPE_RANGE,             // Range-based specialization
    SPEC_TYPE_PATTERN,           // Pattern-based specialization
    SPEC_TYPE_HARDWARE,          // Hardware-specific specialization
    SPEC_TYPE_HOTPATH,           // Hot path specialization
    SPEC_TYPE_INLINE_CACHE,      // Inline caching
    SPEC_TYPE_DEVIRTUALIZATION,  // Virtual call devirtualization
    SPEC_TYPE_LOOP_IDIOM,        // Loop idiom recognition
    SPEC_TYPE_VECTORIZATION      // Auto-vectorization
} SpecializationType;

// Specialization parameter
typedef struct SpecializationParam {
    char* param_name;
    union {
        int64_t constant_value;
        Type* type_value;
        struct {
            int64_t min;
            int64_t max;
        } range_value;
        char* pattern_value;
        HardwareProfile* hardware_value;
    } value;
    SpecializationType type;
    float specialization_benefit;  // Estimated benefit (0.0-1.0)
} SpecializationParam;

// Specialization key (for cache lookup)
typedef struct SpecializationKey {
    char* function_name;
    SpecializationParam* params;
    size_t param_count;
    uint64_t hash;               // Precomputed hash for fast lookup
} SpecializationKey;

// Specialized function instance
typedef struct SpecializedFunction {
    SpecializationKey* key;
    ASTNode* original_function;
    ASTNode* specialized_ast;
    
    // Performance metrics
    double specialization_time;   // Time to generate specialization
    double avg_execution_time;    // Average execution time
    size_t call_count;           // Number of times called
    size_t code_size;            // Size of generated code
    
    // Optimization metadata
    bool is_inlined;
    bool is_vectorized;
    bool is_unrolled;
    int unroll_factor;
    bool uses_hardware_intrinsics;
    
    // Cost-benefit analysis
    float benefit_score;         // Overall benefit score
    float compilation_cost;      // Cost of specialization
    
    time_t created_at;
    time_t last_used;
    
    struct SpecializedFunction* next;
} SpecializedFunction;

// Specialization cache
typedef struct SpecializationCache {
    SpecializedFunction** buckets;
    size_t bucket_count;
    size_t total_entries;
    size_t max_entries;
    
    // Cache statistics
    size_t hits;
    size_t misses;
    size_t evictions;
    
    // Eviction policy
    enum {
        EVICT_LRU,               // Least recently used
        EVICT_LFU,               // Least frequently used
        EVICT_COST_BENEFIT       // Based on cost-benefit analysis
    } eviction_policy;
} SpecializationCache;

// Specialization heuristics
typedef struct SpecializationHeuristics {
    // Thresholds for specialization
    float min_benefit_threshold;      // Minimum benefit to specialize
    size_t min_call_count;           // Minimum calls before specialization
    double max_compilation_time;      // Maximum time for specialization
    size_t max_code_size_increase;    // Maximum code size increase
    
    // Specialization strategies
    bool enable_aggressive_inlining;
    bool enable_constant_propagation;
    bool enable_range_analysis;
    bool enable_type_specialization;
    bool enable_pattern_matching;
    bool enable_hardware_specific;
    
    // Limits
    size_t max_specializations_per_function;
    size_t max_inline_depth;
    size_t max_unroll_factor;
} SpecializationHeuristics;

// Specialization context
typedef struct SpecializationContext {
    SpecializationCache* cache;
    SpecializationHeuristics* heuristics;
    HardwareAwareContext* hardware_context;
    
    // Analysis data
    struct {
        uint64_t* call_counts;       // Function call counts
        double* execution_times;      // Function execution times
        Type** type_profiles;         // Runtime type information
        int64_t** value_profiles;     // Common values for parameters
    } profile_data;
    
    // Active specializations
    SpecializedFunction* active_specializations;
    size_t active_count;
    
    // Statistics
    size_t total_specializations;
    double total_specialization_time;
    double total_benefit;
} SpecializationContext;

// Function declarations

// Context management
SpecializationContext* specialization_context_create(void);
void specialization_context_free(SpecializationContext* context);
bool specialization_context_initialize(SpecializationContext* context);

// Specialization analysis
bool should_specialize_function(SpecializationContext* context,
                               const char* function_name,
                               const SpecializationParam* params,
                               size_t param_count);
float estimate_specialization_benefit(SpecializationContext* context,
                                    ASTNode* function,
                                    const SpecializationParam* params,
                                    size_t param_count);

// Specialization generation
SpecializedFunction* generate_specialization(SpecializationContext* context,
                                           ASTNode* function,
                                           const SpecializationParam* params,
                                           size_t param_count);
ASTNode* specialize_function_body(ASTNode* function,
                                const SpecializationParam* params,
                                size_t param_count);

// Constant propagation and folding
bool propagate_constants(ASTNode* ast, const SpecializationParam* params, size_t param_count);
bool fold_constant_expressions(ASTNode* ast);
bool eliminate_dead_code(ASTNode* ast);

// Type specialization
bool specialize_for_types(ASTNode* ast, const Type** param_types, size_t param_count);
bool devirtualize_calls(ASTNode* ast, const Type** receiver_types);
bool inline_type_checks(ASTNode* ast);

// Range analysis
bool specialize_for_ranges(ASTNode* ast, const SpecializationParam* params, size_t param_count);
bool eliminate_bounds_checks(ASTNode* ast, const SpecializationParam* params);
bool strength_reduce_operations(ASTNode* ast);

// Pattern recognition
bool recognize_loop_idioms(ASTNode* ast);
bool specialize_string_operations(ASTNode* ast);
bool specialize_collection_operations(ASTNode* ast);

// Hardware specialization
bool specialize_for_hardware(ASTNode* ast, HardwareProfile* profile);
bool auto_vectorize_loops(ASTNode* ast, int vector_width);
bool insert_prefetch_instructions(ASTNode* ast);

// Inline caching
typedef struct InlineCache {
    Type* cached_type;
    void* cached_function;
    size_t hit_count;
    size_t miss_count;
} InlineCache;

InlineCache* create_inline_cache(void);
bool update_inline_cache(InlineCache* cache, Type* type, void* function);
void* lookup_inline_cache(InlineCache* cache, Type* type);

// Cache management
SpecializationCache* specialization_cache_create(size_t capacity);
void specialization_cache_free(SpecializationCache* cache);
SpecializedFunction* cache_lookup(SpecializationCache* cache, const SpecializationKey* key);
bool cache_insert(SpecializationCache* cache, SpecializedFunction* specialized);
bool cache_evict_if_needed(SpecializationCache* cache);

// Key management
SpecializationKey* create_specialization_key(const char* function_name,
                                           const SpecializationParam* params,
                                           size_t param_count);
void free_specialization_key(SpecializationKey* key);
uint64_t hash_specialization_key(const SpecializationKey* key);
bool specialization_keys_equal(const SpecializationKey* a, const SpecializationKey* b);

// Cost-benefit analysis
float calculate_benefit_score(const SpecializedFunction* specialized);
float calculate_compilation_cost(size_t ast_size, const SpecializationParam* params);
bool is_specialization_profitable(float benefit, float cost);

// Profile-guided specialization
void update_call_profile(SpecializationContext* context, 
                        const char* function_name,
                        const Type** arg_types,
                        const void** arg_values);
void analyze_hot_paths(SpecializationContext* context);
void suggest_specializations(SpecializationContext* context);

// Runtime specialization
typedef struct RuntimeSpecializer {
    SpecializationContext* context;
    void* jit_context;           // JIT compiler context
    bool enable_lazy_compilation;
    size_t compilation_threshold;
} RuntimeSpecializer;

RuntimeSpecializer* runtime_specializer_create(SpecializationContext* context);
void* runtime_specialize_function(RuntimeSpecializer* specializer,
                                const char* function_name,
                                const Type** arg_types,
                                const void** arg_values);
void runtime_specializer_free(RuntimeSpecializer* specializer);

// Compile-time specialization directives
ComptimeValue* comptime_specialize_for(ComptimeValue* function, ComptimeValue* params);
ComptimeValue* comptime_force_inline(ComptimeValue* function);
ComptimeValue* comptime_no_specialize(ComptimeValue* function);
ComptimeValue* comptime_specialize_aggressive(ComptimeValue* function);

// Debug and statistics
void print_specialization_stats(const SpecializationContext* context);
void print_specialized_function(const SpecializedFunction* specialized);
char* get_specialization_report(const SpecializationContext* context);
void dump_specialization_cache(const SpecializationCache* cache);

// Examples of automatic specializations:

/*
// Original function
func sort[T](arr: []T, cmp: func(T, T) bool) {
    // Generic sorting implementation
}

// Automatically generated specializations:
// 1. Type specialization for int with default comparison
func sort_int_ascending(arr: []int) {
    // Specialized implementation with inlined comparison
    // Vectorized comparison operations
    // Branch prediction hints
}

// 2. Small array specialization
func sort_small_array[T](arr: []T, cmp: func(T, T) bool) 
    where len(arr) <= 8 {
    // Sorting network implementation
    // No loops, fully unrolled
}

// 3. Hardware-specific specialization
func sort_avx512_int32(arr: []int32) {
    // AVX-512 vectorized quicksort
    // 16-wide SIMD operations
}
*/

#endif // CODE_SPECIALIZATION_H