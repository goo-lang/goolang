#ifndef PROFILE_GUIDED_OPTIMIZATION_H
#define PROFILE_GUIDED_OPTIMIZATION_H

#include "optimization.h"
#include "comptime.h"
#include "ast.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct ProfileCollector ProfileCollector;
typedef struct ProfileData ProfileData;
typedef struct BranchInfo BranchInfo;
typedef struct FunctionProfile FunctionProfile;
typedef struct LoopProfile LoopProfile;
typedef struct CallSiteProfile CallSiteProfile;

// =============================================================================
// Profile Data Structures
// =============================================================================

// Branch profiling information
typedef struct BranchInfo {
    char* location;             // Source location (file:line:column)
    uint64_t taken_count;       // Number of times branch was taken
    uint64_t not_taken_count;   // Number of times branch was not taken
    double taken_probability;   // Calculated probability
    struct BranchInfo* next;    // For linked lists
} BranchInfo;

// Function execution profile
typedef struct FunctionProfile {
    char* function_name;        // Function identifier
    uint64_t call_count;        // Number of times called
    uint64_t total_cycles;      // Total execution time in cycles
    uint64_t average_cycles;    // Average execution time
    uint64_t max_cycles;        // Maximum execution time
    uint64_t min_cycles;        // Minimum execution time
    double hotness_score;       // Relative hotness (0.0 to 1.0)
    BranchInfo* branches;       // Branch information within function
    struct FunctionProfile* next;
} FunctionProfile;

// Loop execution profile
typedef struct LoopProfile {
    char* location;             // Loop location
    uint64_t iteration_count;   // Total iterations across all invocations
    uint64_t invocation_count;  // Number of times loop was entered
    double average_iterations;  // Average iterations per invocation
    uint64_t max_iterations;    // Maximum iterations in single invocation
    bool is_vectorizable;       // Whether loop can be vectorized
    struct LoopProfile* next;
} LoopProfile;

// Call site profiling information
typedef struct CallSiteProfile {
    char* location;             // Call site location
    char* target_function;      // Called function name
    uint64_t call_count;        // Number of calls from this site
    double call_frequency;      // Relative frequency
    bool is_indirect;           // Whether it's an indirect call
    struct CallSiteProfile* next;
} CallSiteProfile;

// Complete profile data for a compilation unit
typedef struct ProfileData {
    char* source_file;          // Source file path
    uint64_t total_samples;     // Total profiling samples
    double collection_time;     // Time spent collecting (seconds)
    
    FunctionProfile* functions; // Function profiles
    LoopProfile* loops;         // Loop profiles
    CallSiteProfile* call_sites; // Call site profiles
    
    // Global statistics
    uint64_t total_instructions;
    uint64_t total_branches;
    uint64_t total_mispredicts;
    double branch_prediction_accuracy;
    
    // Optimization hints
    char** hot_functions;       // Functions that should be optimized for speed
    size_t hot_function_count;
    char** cold_functions;      // Functions that can be optimized for size
    size_t cold_function_count;
} ProfileData;

// =============================================================================
// Profile Collection
// =============================================================================

// Profile collection modes
typedef enum {
    PROFILE_MODE_SAMPLING,      // Statistical sampling
    PROFILE_MODE_INSTRUMENTATION, // Code instrumentation
    PROFILE_MODE_HARDWARE,      // Hardware performance counters
    PROFILE_MODE_HYBRID         // Combination of methods
} ProfileMode;

// Profile collector configuration
typedef struct ProfileCollector {
    ProfileMode mode;           // Collection mode
    double sampling_rate;       // For sampling mode (samples per second)
    bool collect_branches;      // Whether to collect branch information
    bool collect_loops;         // Whether to collect loop information
    bool collect_call_sites;    // Whether to collect call site information
    bool collect_memory;        // Whether to collect memory access patterns
    
    // Output configuration
    char* output_file;          // Where to write profile data
    bool binary_format;         // Whether to use binary format
    bool compress_output;       // Whether to compress output
    
    // Runtime state
    ProfileData* current_data;  // Currently collected data
    bool is_collecting;         // Whether collection is active
    uint64_t start_time;        // Collection start time
} ProfileCollector;

// =============================================================================
// Profile-Guided Optimization Engine
// =============================================================================

// PGO optimization strategies
typedef enum {
    PGO_STRATEGY_BRANCH_PREDICTION, // Optimize branch prediction
    PGO_STRATEGY_FUNCTION_LAYOUT,   // Optimize function layout
    PGO_STRATEGY_INLINING,          // Profile-guided inlining
    PGO_STRATEGY_LOOP_OPTIMIZATION, // Loop-specific optimizations
    PGO_STRATEGY_REGISTER_ALLOCATION, // Register allocation hints
    PGO_STRATEGY_INSTRUCTION_SCHEDULING // Instruction scheduling
} PGOStrategy;

// PGO optimization context
typedef struct PGOContext {
    ProfileData* profile_data;   // Input profile data
    OptimizationContext* opt_ctx; // Base optimization context
    
    // Configuration
    double confidence_threshold; // Minimum confidence for optimizations
    bool enable_aggressive;     // Enable aggressive optimizations
    PGOStrategy* enabled_strategies; // Enabled optimization strategies
    size_t strategy_count;
    
    // Generated optimizations
    char** optimization_code;   // Generated optimization directives
    size_t optimization_count;
    size_t optimization_capacity;
} PGOContext;

// =============================================================================
// Core PGO Functions
// =============================================================================

// Profile collector management
ProfileCollector* profile_collector_new(ProfileMode mode);
void profile_collector_free(ProfileCollector* collector);
int profile_collector_start(ProfileCollector* collector, const char* target_program);
int profile_collector_stop(ProfileCollector* collector);
int profile_collector_save(ProfileCollector* collector, const char* filename);

// Profile data management
ProfileData* profile_data_new(const char* source_file);
void profile_data_free(ProfileData* data);
ProfileData* profile_data_load(const char* filename);
int profile_data_save(ProfileData* data, const char* filename);
ProfileData* profile_data_merge(ProfileData* data1, ProfileData* data2);

// Profile analysis
double profile_function_hotness(ProfileData* data, const char* function_name);
double profile_branch_probability(ProfileData* data, const char* location);
uint64_t profile_loop_average_iterations(ProfileData* data, const char* location);
bool profile_is_function_hot(ProfileData* data, const char* function_name, double threshold);
bool profile_is_function_cold(ProfileData* data, const char* function_name, double threshold);

// PGO optimization engine
PGOContext* pgo_context_new(ProfileData* profile_data, OptimizationContext* opt_ctx);
void pgo_context_free(PGOContext* ctx);
int pgo_context_add_strategy(PGOContext* ctx, PGOStrategy strategy);
int pgo_context_set_confidence_threshold(PGOContext* ctx, double threshold);

// PGO optimization generation
ComptimeResult* pgo_optimize_function(PGOContext* ctx, const char* function_name, ASTNode* func_node);
ComptimeResult* pgo_optimize_branch(PGOContext* ctx, const char* location, ASTNode* branch_node);
ComptimeResult* pgo_optimize_loop(PGOContext* ctx, const char* location, ASTNode* loop_node);
ComptimeResult* pgo_optimize_call_site(PGOContext* ctx, const char* location, ASTNode* call_node);

// PGO integration with compile-time system
ComptimeResult* comptime_pgo_analyze(ComptimeContext* ctx, const char* profile_file);
ComptimeResult* comptime_pgo_optimize(ComptimeContext* ctx, ASTNode* target, const char* strategy);
ComptimeResult* comptime_pgo_generate_hints(ComptimeContext* ctx, ProfileData* data);

// =============================================================================
// PGO Intrinsics for Compile-Time Use
// =============================================================================

// Profile-guided compile-time intrinsics
ComptimeResult* comptime_intrinsic_profile_hotness(ComptimeContext* ctx, ComptimeValue* function_name);
ComptimeResult* comptime_intrinsic_branch_probability(ComptimeContext* ctx, ComptimeValue* location);
ComptimeResult* comptime_intrinsic_loop_iterations(ComptimeContext* ctx, ComptimeValue* location);
ComptimeResult* comptime_intrinsic_is_hot_function(ComptimeContext* ctx, ComptimeValue* function_name);
ComptimeResult* comptime_intrinsic_is_cold_function(ComptimeContext* ctx, ComptimeValue* function_name);

// Profile data introspection
ComptimeResult* comptime_intrinsic_profile_stats(ComptimeContext* ctx);
ComptimeResult* comptime_intrinsic_profile_functions(ComptimeContext* ctx);
ComptimeResult* comptime_intrinsic_profile_branches(ComptimeContext* ctx);

// =============================================================================
// Utility Functions
// =============================================================================

// Profile data utilities
int profile_add_function(ProfileData* data, const char* name, uint64_t call_count, uint64_t cycles);
int profile_add_branch(ProfileData* data, const char* location, uint64_t taken, uint64_t not_taken);
int profile_add_loop(ProfileData* data, const char* location, uint64_t iterations, uint64_t invocations);
int profile_add_call_site(ProfileData* data, const char* location, const char* target, uint64_t count);

// Profile analysis utilities
void profile_calculate_hotness_scores(ProfileData* data);
void profile_identify_hot_cold_functions(ProfileData* data, double hot_threshold, double cold_threshold);
void profile_calculate_branch_probabilities(ProfileData* data);

// PGO code generation utilities
char* pgo_generate_branch_hints(BranchInfo* branch);
char* pgo_generate_function_attributes(FunctionProfile* func);
char* pgo_generate_loop_pragmas(LoopProfile* loop);
char* pgo_generate_inline_hints(CallSiteProfile* call_site);

// Debug and testing utilities
void profile_print_summary(ProfileData* data);
void profile_print_function_stats(ProfileData* data, const char* function_name);
void profile_print_hot_functions(ProfileData* data, size_t count);
int profile_validate_data(ProfileData* data);

#endif // PROFILE_GUIDED_OPTIMIZATION_H
