#ifndef AUTO_PARALLEL_H
#define AUTO_PARALLEL_H

#include "ast.h"
#include "types.h"
#include "comptime.h"
#include <stdbool.h>
#include <stdint.h>

// Forward declarations
typedef struct ParallelContext ParallelContext;
typedef struct DependencyGraph DependencyGraph;
typedef struct LoopInfo LoopInfo;
typedef struct ParallelAnnotation ParallelAnnotation;

// Parallelization annotation types
typedef enum {
    PARALLEL_AUTO,          // @auto_parallel - automatic decision
    PARALLEL_FORCE,         // @parallel - force parallelization
    PARALLEL_SIMD,          // @simd - force SIMD vectorization
    PARALLEL_TASK,          // @task - task-based parallelism
    PARALLEL_SEQUENTIAL,    // @sequential - force sequential execution
    PARALLEL_REDUCTION,     // @reduction(op) - parallel reduction
    PARALLEL_PIPELINE,      // @pipeline - pipeline parallelism
    PARALLEL_COUNT
} ParallelAnnotationType;

// Reduction operation types
typedef enum {
    REDUCTION_SUM,          // Sum reduction (+)
    REDUCTION_PRODUCT,      // Product reduction (*)
    REDUCTION_MIN,          // Minimum value
    REDUCTION_MAX,          // Maximum value
    REDUCTION_AND,          // Bitwise AND
    REDUCTION_OR,           // Bitwise OR
    REDUCTION_XOR,          // Bitwise XOR
    REDUCTION_CUSTOM,       // User-defined reduction
    REDUCTION_COUNT
} ReductionType;

// Hardware capability flags
typedef enum {
    HW_CAP_SSE = 1 << 0,        // SSE instruction set
    HW_CAP_AVX = 1 << 1,        // AVX instruction set
    HW_CAP_AVX2 = 1 << 2,       // AVX2 instruction set
    HW_CAP_AVX512 = 1 << 3,     // AVX-512 instruction set
    HW_CAP_NEON = 1 << 4,       // ARM NEON
    HW_CAP_SVE = 1 << 5,        // ARM SVE
    HW_CAP_MULTICORE = 1 << 6,  // Multi-core support
    HW_CAP_NUMA = 1 << 7,       // NUMA awareness
    HW_CAP_HYPERTHREADING = 1 << 8  // Hyper-threading support
} HardwareCapability;

// Parallelization strategy
typedef enum {
    STRATEGY_NONE,          // No parallelization
    STRATEGY_LOOP_PARALLEL, // Loop-level parallelization
    STRATEGY_TASK_PARALLEL, // Task-based parallelism
    STRATEGY_SIMD,          // SIMD vectorization
    STRATEGY_PIPELINE,      // Pipeline parallelism
    STRATEGY_HYBRID,        // Combination of strategies
    STRATEGY_COUNT
} ParallelizationStrategy;

// Performance cost model parameters
typedef struct CostModel {
    double computation_cost;    // Cost of computation per element
    double memory_cost;         // Cost of memory access per element
    double synchronization_cost; // Cost of synchronization overhead
    double setup_cost;          // Cost of setting up parallelization
    size_t threshold_size;      // Minimum size for parallelization
    double parallel_efficiency; // Expected parallel efficiency
} CostModel;

// Parallelization annotation
typedef struct ParallelAnnotation {
    ParallelAnnotationType type;    // Annotation type
    ReductionType reduction_op;     // Reduction operation (if applicable)
    char* custom_reduction_func;    // Custom reduction function name
    int chunk_size;                 // Chunk size for parallel loops
    int num_threads;                // Number of threads (0 = auto)
    bool vectorize;                 // Enable vectorization
    bool schedule_static;           // Use static scheduling
    int unroll_factor;              // Loop unroll factor
    char** dependencies;            // Dependency specifications
    size_t dependency_count;        // Number of dependencies
    CostModel* cost_model;          // Custom cost model
} ParallelAnnotation;

// Loop analysis information
typedef struct LoopInfo {
    ASTNode* loop_node;             // AST node for the loop
    ASTNode* init_stmt;             // Loop initialization
    ASTNode* condition;             // Loop condition
    ASTNode* increment;             // Loop increment
    ASTNode* body;                  // Loop body
    
    // Analysis results
    bool is_countable;              // Has known iteration count
    int64_t iteration_count;        // Number of iterations (if known)
    bool has_dependencies;          // Has loop-carried dependencies
    bool is_vectorizable;           // Can be vectorized
    bool is_parallelizable;         // Can be parallelized
    
    // Memory access patterns
    char** array_accesses;          // Array variables accessed
    size_t access_count;            // Number of array accesses
    bool has_indirect_access;       // Has indirect memory access
    bool has_constant_stride;       // Has constant stride access
    
    // Nested loop information
    struct LoopInfo* parent;        // Parent loop (if nested)
    struct LoopInfo** children;     // Child loops
    size_t child_count;             // Number of child loops
    int nesting_level;              // Nesting level (0 = outermost)
} LoopInfo;

// Variable access types
typedef enum {
    ACCESS_READ,        // Variable is read
    ACCESS_WRITE,       // Variable is written
    ACCESS_READ_WRITE   // Variable is both read and written
} VariableAccessType;

// Variable access information
typedef struct VariableAccess {
    char* variable_name;        // Name of the accessed variable
    VariableAccessType access_type; // Type of access (read/write)
    char* array_index;          // Array index expression (if applicable)
    bool is_indirect;           // True for pointer dereferences
    bool is_constant_index;     // True if array index is constant
    void* memory_address;       // Memory address (for analysis)
} VariableAccess;

// Data dependency types
typedef enum {
    DEP_NONE,           // No dependency
    DEP_TRUE,           // True dependency (RAW)
    DEP_ANTI,           // Anti dependency (WAR)
    DEP_OUTPUT,         // Output dependency (WAW)
    DEP_INPUT,          // Input dependency (RAR)
    DEP_CONTROL,        // Control dependency
    DEP_UNKNOWN         // Unknown dependency
} DependencyType;

// Data dependency node
typedef struct DependencyNode {
    ASTNode* statement;             // Statement that creates dependency
    int statement_id;               // Statement identifier
    DependencyType dep_type;        // Type of dependency
    char* variable_name;            // Variable involved in dependency
    int dependency_distance;        // Loop distance (for loop dependencies)
    VariableAccess* source_access;  // Source variable access
    VariableAccess* target_access;  // Target variable access
    struct DependencyNode* next;   // Next dependency in list
} DependencyNode;

// Dependency graph
typedef struct DependencyGraph {
    DependencyNode** nodes;         // Array of dependency nodes
    size_t node_count;              // Number of nodes
    size_t capacity;                // Capacity of nodes array
    bool has_cycles;                // Whether graph has cycles
    int** adjacency_matrix;         // Adjacency matrix for dependencies
} DependencyGraph;

// Hardware detection and capabilities
typedef struct HardwareInfo {
    HardwareCapability capabilities;    // Available hardware capabilities
    int num_cores;                      // Number of CPU cores
    int num_threads;                    // Number of hardware threads
    size_t cache_line_size;             // Cache line size in bytes
    size_t l1_cache_size;               // L1 cache size
    size_t l2_cache_size;               // L2 cache size
    size_t l3_cache_size;               // L3 cache size
    int simd_width;                     // SIMD register width in bytes
    bool numa_available;                // NUMA topology available
    int numa_nodes;                     // Number of NUMA nodes
} HardwareInfo;

// Parallelization decision
typedef struct ParallelDecision {
    ParallelizationStrategy strategy;   // Chosen parallelization strategy
    bool should_parallelize;            // Whether to parallelize
    double expected_speedup;            // Expected speedup factor
    int recommended_threads;            // Recommended number of threads
    int chunk_size;                     // Recommended chunk size
    bool use_simd;                      // Whether to use SIMD
    char* reasoning;                    // Human-readable reasoning
    
    // Code generation parameters
    bool generate_fallback;             // Generate sequential fallback
    bool profile_guided;                // Use profile-guided optimization
    int unroll_factor;                  // Loop unroll factor
    bool prefetch_data;                 // Insert prefetch instructions
} ParallelDecision;

// Main parallelization context
typedef struct ParallelContext {
    // Hardware information
    HardwareInfo* hw_info;              // Hardware capabilities
    
    // Analysis state
    DependencyGraph* dep_graph;         // Current dependency graph
    LoopInfo** loop_stack;              // Stack of nested loops
    size_t loop_depth;                  // Current loop nesting depth
    
    // Configuration
    bool aggressive_mode;               // Aggressive parallelization
    bool conservative_mode;             // Conservative parallelization
    double threshold_benefit;           // Minimum benefit threshold
    int max_threads;                    // Maximum threads to use
    
    // Statistics
    size_t loops_analyzed;              // Number of loops analyzed
    size_t loops_parallelized;          // Number of loops parallelized
    size_t functions_parallelized;      // Number of functions parallelized
    
    // Integration with other systems
    ComptimeContext* comptime_ctx;      // Compile-time execution context
    TypeChecker* type_checker;          // Type checker instance
} ParallelContext;

// Core parallelization functions
ParallelContext* parallel_context_new(HardwareInfo* hw_info);
void parallel_context_free(ParallelContext* ctx);

// Hardware detection
HardwareInfo* detect_hardware_capabilities(void);
void hardware_info_free(HardwareInfo* hw_info);
bool hardware_supports_capability(HardwareInfo* hw_info, HardwareCapability cap);

// Annotation parsing and analysis
ParallelAnnotation* parse_parallel_annotation(ASTNode* annotation_node);
void parallel_annotation_free(ParallelAnnotation* annotation);
bool validate_parallel_annotation(ParallelAnnotation* annotation, ASTNode* target);

// AST analysis for parallelization opportunities
bool analyze_function_for_parallelization(ParallelContext* ctx, ASTNode* function);
LoopInfo* analyze_loop(ParallelContext* ctx, ASTNode* loop_node);
void loop_info_free(LoopInfo* loop_info);

// Data dependency analysis
DependencyGraph* build_dependency_graph(ParallelContext* ctx, ASTNode* code_block);
bool has_loop_carried_dependency(DependencyGraph* graph, LoopInfo* loop);
DependencyType analyze_statement_dependency(ASTNode* stmt1, ASTNode* stmt2);
void dependency_graph_free(DependencyGraph* graph);

// Variable access analysis helper functions
void extract_variable_accesses(ASTNode* stmt, VariableAccess** reads, size_t* read_count,
                               VariableAccess** writes, size_t* write_count);
void extract_accesses_recursive(ASTNode* node, VariableAccess** reads, size_t* read_count, size_t* read_capacity,
                                VariableAccess** writes, size_t* write_count, size_t* write_capacity,
                                bool is_lvalue);
bool variables_may_alias(VariableAccess* access1, VariableAccess* access2);
void free_variable_accesses(VariableAccess* accesses, size_t count);
bool is_loop_variant_access(VariableAccess* access, LoopInfo* loop);
bool can_prove_no_cross_iteration_dependence(DependencyNode* dep_node, LoopInfo* loop);
void analyze_code_block_dependencies(DependencyGraph* graph, ASTNode* code_block);
void add_statement_to_graph(DependencyGraph* graph, ASTNode* stmt, size_t index);
void add_dependency_edge(DependencyGraph* graph, size_t from_index, size_t to_index, DependencyType dep_type);

// Parallelization decision making
ParallelDecision* make_parallelization_decision(ParallelContext* ctx, 
                                              LoopInfo* loop, 
                                              ParallelAnnotation* annotation);
double estimate_parallelization_benefit(ParallelContext* ctx, LoopInfo* loop, 
                                       ParallelizationStrategy strategy);
bool is_parallelization_profitable(ParallelContext* ctx, LoopInfo* loop, 
                                  ParallelDecision* decision);

// Cost model functions
CostModel* create_default_cost_model(void);
CostModel* create_adaptive_cost_model(HardwareInfo* hw_info);
void cost_model_free(CostModel* model);
double calculate_parallel_cost(CostModel* model, size_t problem_size, int num_threads);

// Loop transformation utilities
ASTNode* transform_loop_for_parallelization(ParallelContext* ctx, LoopInfo* loop, 
                                          ParallelDecision* decision);
ASTNode* generate_parallel_loop(ParallelContext* ctx, LoopInfo* loop, 
                               ParallelDecision* decision);
ASTNode* generate_simd_loop(ParallelContext* ctx, LoopInfo* loop);
ASTNode* generate_task_parallel_code(ParallelContext* ctx, ASTNode* function);

// Integration functions
bool integrate_with_compiler_pipeline(ParallelContext* ctx);
void register_parallel_optimizations(ParallelContext* ctx);
bool apply_parallelization_to_module(ParallelContext* ctx, ASTNode* module);

// Debugging and analysis utilities
void print_parallelization_report(ParallelContext* ctx);
void print_dependency_graph(DependencyGraph* graph);
void print_loop_analysis(LoopInfo* loop);
char* get_parallelization_summary(ParallelContext* ctx);

// Configuration functions
void set_parallelization_aggressiveness(ParallelContext* ctx, double level);
void set_thread_limit(ParallelContext* ctx, int max_threads);
void enable_profiling_mode(ParallelContext* ctx, bool enabled);

// Utility functions
const char* parallel_strategy_string(ParallelizationStrategy strategy);
const char* dependency_type_string(DependencyType type);
const char* reduction_type_string(ReductionType type);
bool is_reduction_operation(ASTNode* expr, ReductionType* detected_type);

// Performance Models and Integration (Task 29.6)
typedef struct ParallelizationConfig {
    double aggressiveness_level;    // 0.0 = conservative, 1.0 = aggressive
    double benefit_threshold;       // Minimum speedup threshold
    size_t min_problem_size;        // Minimum problem size for parallelization
    int max_threads;               // Maximum threads to use
    bool enable_warnings;           // Enable compile-time warnings
    bool enable_monitoring;         // Enable runtime monitoring
    bool enable_simd;              // Enable SIMD vectorization
    bool enable_task_parallelism;   // Enable task-based parallelism
} ParallelizationConfig;

// Performance monitoring functions
void init_performance_monitoring(const char* log_filename);
void record_performance_metrics(double predicted_speedup, double actual_speedup,
                               size_t iterations, double seq_time, double par_time,
                               double overhead, int threads);
void analyze_prediction_accuracy(ParallelContext* ctx);
void cleanup_performance_monitoring(void);

// Enhanced cost model with warnings
double calculate_parallel_benefit_with_warnings(ParallelContext* ctx, LoopInfo* loop,
                                               ParallelDecision* decision);

// Configuration functions
void configure_parallelization(ParallelizationConfig* config);
ParallelizationConfig* get_parallelization_config(void);
bool should_parallelize_with_config(ParallelContext* ctx, LoopInfo* loop,
                                   ParallelDecision* decision);

// Integration functions
void integrate_parallelization_with_llvm(ParallelContext* ctx, void* llvm_module);
void enable_parallelization_profiling(const char* profile_file);
void generate_performance_report(ParallelContext* ctx, const char* report_file);

#endif // AUTO_PARALLEL_H