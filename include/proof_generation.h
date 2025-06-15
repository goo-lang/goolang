#ifndef PROOF_GENERATION_H
#define PROOF_GENERATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "ast.h"
#include "types.h"
#include "contracts.h"
#include "dependent_types.h"

#include "panic_free.h"  // For existing ProofStatus

// C23 compatibility
_Static_assert(sizeof(bool) == 1, "bool should be 1 byte");

// =============================================================================
// Proof System Types
// =============================================================================

// Types of proofs we can generate
typedef enum {
    PROOF_MEMORY_SAFETY = 0,      // Null pointer, use-after-free, buffer overflow
    PROOF_BOUNDS_CHECKING,        // Array access bounds
    PROOF_TERMINATION,            // Loop and recursion termination
    PROOF_FUNCTIONAL_CORRECTNESS, // Function behavior matches specification
    PROOF_INVARIANT_PRESERVATION, // Loop invariants are maintained
    PROOF_DEADLOCK_FREEDOM,       // No deadlocks in concurrent code
    PROOF_RESOURCE_SAFETY,        // Proper resource cleanup
    PROOF_TYPE_SAFETY,            // Type system correctness
    PROOF_TYPE_COUNT
} ProofType;

// Proof verification status (extending panic_free.h ProofStatus)
// We reuse PROOF_STATUS_UNKNOWN, PROOF_STATUS_TIMEOUT from panic_free.h
// and add new status values as needed
#define PROOF_STATUS_VERIFIED PROOF_STATUS_SAFE
#define PROOF_STATUS_FAILED PROOF_STATUS_UNSAFE  
#define PROOF_STATUS_INCOMPLETE PROOF_STATUS_CONDITIONAL
#define PROOF_STATUS_CACHED PROOF_STATUS_SAFE  // Treat cached as safe

// SMT solver backends
typedef enum {
    SMT_SOLVER_Z3 = 0,
    SMT_SOLVER_CVC5,
    SMT_SOLVER_YICES,
    SMT_SOLVER_MATHSAT,
    SMT_SOLVER_COUNT
} SMTSolver;

// SMT solver result types
typedef enum {
    SMT_RESULT_SAT = 0,      // Formula is satisfiable
    SMT_RESULT_UNSAT,        // Formula is unsatisfiable  
    SMT_RESULT_UNKNOWN,      // Result is unknown
    SMT_RESULT_TIMEOUT,      // Solver timed out
    SMT_RESULT_ERROR         // Error occurred
} SMTResult;

// Abstract interpretation domains for invariant inference
typedef enum {
    ABSTRACT_DOMAIN_INTERVALS = 0, // Numeric intervals
    ABSTRACT_DOMAIN_SIGNS,         // Sign analysis
    ABSTRACT_DOMAIN_SHAPES,        // Heap shape analysis
    ABSTRACT_DOMAIN_POINTS_TO,     // Points-to analysis
    ABSTRACT_DOMAIN_OCTAGON,       // Octagonal constraints
    ABSTRACT_DOMAIN_POLYHEDRA,     // Polyhedral constraints
    ABSTRACT_DOMAIN_COUNT
} AbstractDomain;

// =============================================================================
// Proof Data Structures
// =============================================================================

// Mathematical expression for SMT encoding
typedef struct SMTExpression {
    enum {
        SMT_VAR,          // Variable
        SMT_CONST,        // Constant
        SMT_APP,          // Function application
        SMT_QUANTIFIER,   // Forall/exists quantifier
        SMT_BOOLEAN,      // Boolean expression
        SMT_ARITHMETIC,   // Arithmetic expression
        SMT_ARRAY,        // Array expression
        SMT_BITVECTOR     // Bit-vector expression
    } type;
    
    union {
        struct {
            char* name;
            struct Type* var_type;
        } variable;
        
        struct {
            union {
                int64_t int_val;
                double float_val;
                bool bool_val;
                char* string_val;
            };
            struct Type* const_type;
        } constant;
        
        struct {
            char* function_name;
            struct SMTExpression** args;
            size_t arg_count;
        } application;
        
        struct {
            enum { SMT_FORALL, SMT_EXISTS } quantifier_type;
            char** bound_vars;
            struct Type** var_types;
            size_t var_count;
            struct SMTExpression* body;
        } quantifier;
    };
    
    struct SMTExpression* next;
} SMTExpression;

// Proof obligation - something that needs to be proven
typedef struct ProofObligation {
    ProofType proof_type;
    char* description;
    struct ASTNode* source_location;
    
    // SMT encoding of the property to prove
    SMTExpression* preconditions;     // What we assume to be true
    SMTExpression* postconditions;    // What we need to prove
    SMTExpression* invariants;        // Loop invariants (if applicable)
    
    // Proof context
    struct {
        int line;
        int column;
        char* filename;
        char* function_name;
    };
    
    // Verification results
    ProofStatus status;
    double verification_time;
    char* proof_trace;               // Detailed proof steps
    char* counterexample;            // If proof failed
    
    struct ProofObligation* next;
} ProofObligation;

// Loop termination ranking function
typedef struct TerminationMeasure {
    SMTExpression* ranking_function;  // Must decrease on each iteration
    SMTExpression* bound_condition;   // Must be non-negative
    char* termination_argument;       // Human-readable explanation
} TerminationMeasure;

// Automatic invariant inference result
typedef struct InferredInvariant {
    SMTExpression* invariant_expr;
    AbstractDomain domain_used;
    double confidence_score;          // 0.0 to 1.0
    char* inference_method;
    struct InferredInvariant* next;
} InferredInvariant;

// Memory safety analysis result
typedef struct MemorySafetyProof {
    struct {
        bool null_pointer_safe;
        bool buffer_overflow_safe;
        bool use_after_free_safe;
        bool double_free_safe;
        bool memory_leak_safe;
    } safety_properties;
    
    SMTExpression* memory_model;      // Formal memory model
    char** unsafe_operations;        // List of potentially unsafe operations
    size_t unsafe_count;
} MemorySafetyProof;

// =============================================================================
// Proof Generation Context
// =============================================================================

typedef struct ProofGenerationContext {
    // SMT solver configuration
    SMTSolver solver_backend;
    int solver_timeout_seconds;
    bool use_proof_caching;
    char* cache_directory;
    
    // Abstract interpretation settings
    AbstractDomain* enabled_domains;
    size_t domain_count;
    int max_widening_iterations;
    
    // Proof obligations
    ProofObligation* obligations;
    size_t total_obligations;
    size_t verified_obligations;
    size_t failed_obligations;
    
    // Caching system
    struct {
        struct ProofCache* cache;
        bool cache_hits_enabled;
        size_t cache_hit_count;
        size_t cache_miss_count;
    };
    
    // Statistics
    struct {
        double total_verification_time;
        size_t smt_queries_generated;
        size_t invariants_inferred;
        size_t termination_proofs;
        size_t memory_safety_proofs;
    } statistics;
} ProofGenerationContext;

// Proof cache entry
typedef struct ProofCache {
    char* obligation_hash;            // Hash of the proof obligation
    ProofStatus cached_status;
    char* cached_proof;
    double cached_time;
    time_t cache_timestamp;
    struct ProofCache* next;
} ProofCache;

// =============================================================================
// Core Proof Generation Functions
// =============================================================================

// Create and manage proof generation context
ProofGenerationContext* proof_generation_context_create(void);
void proof_generation_context_free(ProofGenerationContext* ctx);

// Configure proof generation
int proof_generation_configure_solver(ProofGenerationContext* ctx, SMTSolver solver);
int proof_generation_enable_domain(ProofGenerationContext* ctx, AbstractDomain domain);
int proof_generation_set_timeout(ProofGenerationContext* ctx, int seconds);

// Main proof generation entry points
int generate_proofs_for_function(
    ProofGenerationContext* ctx,
    struct ASTNode* function_ast,
    FunctionContract* contracts
);

int generate_proofs_for_program(
    ProofGenerationContext* ctx,
    struct ASTNode* program_ast,
    ContractContext* contract_ctx
);

// =============================================================================
// Memory Safety Proof Generation
// =============================================================================

MemorySafetyProof* generate_memory_safety_proof(
    ProofGenerationContext* ctx,
    struct ASTNode* code_block,
    DependentTypeContext* type_ctx
);

int verify_null_pointer_safety(
    ProofGenerationContext* ctx,
    struct ASTNode* expr,
    SMTExpression** proof_out
);

int verify_buffer_bounds_safety(
    ProofGenerationContext* ctx,
    struct ASTNode* array_access,
    SMTExpression** proof_out
);

int verify_use_after_free_safety(
    ProofGenerationContext* ctx,
    struct ASTNode* pointer_usage,
    SMTExpression** proof_out
);

// =============================================================================
// Termination Proof Generation
// =============================================================================

TerminationMeasure* generate_termination_proof(
    ProofGenerationContext* ctx,
    struct ASTNode* loop_ast,
    InferredInvariant* loop_invariants
);

int find_ranking_function(
    ProofGenerationContext* ctx,
    struct ASTNode* loop_ast,
    SMTExpression** ranking_function_out
);

int verify_ranking_function_decreases(
    ProofGenerationContext* ctx,
    SMTExpression* ranking_function,
    struct ASTNode* loop_body
);

// =============================================================================
// Automatic Invariant Inference
// =============================================================================

InferredInvariant* proof_infer_loop_invariants(
    ProofGenerationContext* ctx,
    struct ASTNode* loop_ast,
    AbstractDomain domain
);

InferredInvariant* infer_invariants_abstract_interpretation(
    ProofGenerationContext* ctx,
    struct ASTNode* loop_ast,
    AbstractDomain domain
);

InferredInvariant* infer_invariants_dynamic_analysis(
    ProofGenerationContext* ctx,
    struct ASTNode* loop_ast,
    int max_iterations
);

// =============================================================================
// SMT Expression Generation and Manipulation
// =============================================================================

SMTExpression* ast_to_smt_expression(
    struct ASTNode* ast,
    DependentTypeContext* type_ctx
);

SMTExpression* contract_to_smt_expression(
    ContractExpression* contract,
    DependentTypeContext* type_ctx
);

char* smt_expression_to_smtlib(SMTExpression* expr);

// SMT expression constructors
SMTExpression* smt_var(const char* name, struct Type* type);
SMTExpression* smt_const_int(int64_t value);
SMTExpression* smt_const_bool(bool value);
SMTExpression* smt_app(const char* function_name, SMTExpression** args, size_t arg_count);
SMTExpression* smt_forall(char** vars, struct Type** types, size_t var_count, SMTExpression* body);
SMTExpression* smt_exists(char** vars, struct Type** types, size_t var_count, SMTExpression* body);

// SMT expression operations
SMTExpression* smt_and(SMTExpression* left, SMTExpression* right);
SMTExpression* smt_or(SMTExpression* left, SMTExpression* right);
SMTExpression* smt_not(SMTExpression* expr);
SMTExpression* smt_implies(SMTExpression* premise, SMTExpression* conclusion);
SMTExpression* smt_equals(SMTExpression* left, SMTExpression* right);
SMTExpression* smt_less_than(SMTExpression* left, SMTExpression* right);

// =============================================================================
// SMT Solver Integration
// =============================================================================

// SMT solver interface
typedef struct SMTSolverInterface {
    SMTSolver solver_type;
    char* solver_path;
    char* working_directory;
    int timeout_seconds;
    
    // Function pointers for solver operations
    int (*initialize)(struct SMTSolverInterface* solver);
    int (*add_assertion)(struct SMTSolverInterface* solver, SMTExpression* expr);
    int (*check_sat)(struct SMTSolverInterface* solver);
    char* (*get_model)(struct SMTSolverInterface* solver);
    char* (*get_proof)(struct SMTSolverInterface* solver);
    void (*cleanup)(struct SMTSolverInterface* solver);
} SMTSolverInterface;

SMTSolverInterface* smt_solver_interface_create(SMTSolver solver_type);
void smt_solver_interface_free(SMTSolverInterface* solver);

int smt_solver_verify_formula(
    SMTSolverInterface* solver,
    SMTExpression* formula,
    char** proof_out,
    char** counterexample_out
);

// =============================================================================
// Error Handling and Diagnostics  
// =============================================================================

typedef struct ProofGenerationError {
    enum {
        PROOF_ERROR_SMT_SOLVER,      // SMT solver error
        PROOF_ERROR_TIMEOUT,         // Verification timeout
        PROOF_ERROR_UNSUPPORTED,     // Unsupported feature
        PROOF_ERROR_INVALID_INPUT,   // Invalid AST or contracts
        PROOF_ERROR_CACHE,           // Cache system error
        PROOF_ERROR_MEMORY,          // Out of memory
        PROOF_ERROR_INVARIANT        // Invariant inference failed
    } error_type;
    
    char* error_message;
    char* error_location;
    ProofObligation* failed_obligation;
    struct ProofGenerationError* next;
} ProofGenerationError;

void report_proof_generation_error(ProofGenerationError* error);
char* format_proof_failure_message(ProofObligation* obligation, const char* reason);

// =============================================================================
// Integration with Contract System
// =============================================================================

int integrate_contracts_with_proofs(
    ProofGenerationContext* proof_ctx,
    ContractContext* contract_ctx,
    DependentTypeContext* type_ctx
);

ProofObligation* contract_to_proof_obligation(
    ContractExpression* contract,
    struct ASTNode* context
);

int verify_contract_with_smt(
    ProofGenerationContext* ctx,
    ContractExpression* contract,
    SMTExpression** proof_out
);

// =============================================================================
// Statistics and Reporting
// =============================================================================

void print_proof_generation_statistics(ProofGenerationContext* ctx);
void generate_proof_report(ProofGenerationContext* ctx, const char* output_file);

typedef struct ProofReport {
    size_t total_functions_analyzed;
    size_t total_proofs_generated;
    size_t memory_safety_proofs;
    size_t termination_proofs;
    size_t functional_correctness_proofs;
    double average_verification_time;
    size_t cache_hit_rate_percent;
    char* detailed_statistics;
} ProofReport;

ProofReport* generate_proof_summary(ProofGenerationContext* ctx);

// =============================================================================
// Memory Management
// =============================================================================

void smt_expression_free(SMTExpression* expr);
void proof_obligation_free(ProofObligation* obligation);
void termination_measure_free(TerminationMeasure* measure);
void inferred_invariant_free(InferredInvariant* invariant);
void memory_safety_proof_free(MemorySafetyProof* proof);
void proof_generation_error_free(ProofGenerationError* error);
void proof_report_free(ProofReport* report);

// =============================================================================
// Utility Functions
// =============================================================================

bool is_smt_expression_valid(SMTExpression* expr);
bool is_proof_obligation_satisfiable(ProofObligation* obligation);
int estimate_proof_complexity(ProofObligation* obligation);
double calculate_confidence_score(InferredInvariant* invariant);

// =============================================================================
// Configuration and Tuning
// =============================================================================

typedef struct ProofGenerationConfig {
    // Solver settings
    SMTSolver preferred_solver;
    int default_timeout;
    bool enable_proof_caching;
    
    // Abstract interpretation settings
    AbstractDomain* inference_domains;
    size_t domain_count;
    int max_widening_iterations;
    
    // Performance tuning
    bool enable_parallel_verification;
    int max_worker_threads;
    size_t memory_limit_mb;
    
    // Output settings
    bool generate_detailed_proofs;
    bool generate_counterexamples;
    char* output_directory;
} ProofGenerationConfig;

ProofGenerationConfig* proof_generation_config_default(void);
int proof_generation_config_load_from_file(const char* config_file, ProofGenerationConfig** config);
void proof_generation_config_free(ProofGenerationConfig* config);

// =============================================================================
// =============================================================================
// Clean Function Declarations
// =============================================================================

// SMT Expression Construction
SMTExpression* smt_var(const char* name, struct Type* type);
SMTExpression* smt_const_int(int64_t value);
SMTExpression* smt_const_bool(bool value);
SMTExpression* smt_app(const char* function_name, SMTExpression** args, size_t arg_count);
SMTExpression* smt_forall(char** var_names, struct Type** var_types, size_t var_count, SMTExpression* body);
SMTExpression* smt_and(SMTExpression* left, SMTExpression* right);
SMTExpression* smt_or(SMTExpression* left, SMTExpression* right);
SMTExpression* smt_not(SMTExpression* expr);
SMTExpression* smt_implies(SMTExpression* left, SMTExpression* right);
SMTExpression* smt_equals(SMTExpression* left, SMTExpression* right);

// SMT solver integration
SMTResult smt_check_satisfiability(ProofGenerationContext* ctx, SMTExpression* formula);
SMTResult invoke_z3_solver(ProofGenerationContext* ctx, const char* smt_query);
SMTResult invoke_cvc5_solver(ProofGenerationContext* ctx, const char* smt_query);
SMTResult invoke_yices_solver(ProofGenerationContext* ctx, const char* smt_query);
char* smt_expression_to_string(SMTExpression* expr);

// Proof generation for specific constructs  
int generate_proof_for_assignment(ProofGenerationContext* ctx, struct ASTNode* assignment_node);
int generate_proof_for_function_call(ProofGenerationContext* ctx, struct ASTNode* call_node);
int generate_proof_for_array_access(ProofGenerationContext* ctx, struct ASTNode* access_node);

// Proof caching
ProofCache* proof_cache_create(const char* cache_dir);
void proof_cache_free(ProofCache* cache);
int proof_cache_store(ProofCache* cache, const char* obligation_hash, ProofStatus status, const char* proof, double verification_time);
int proof_cache_lookup(ProofCache* cache, const char* obligation_hash, ProofStatus* status_out, char** proof_out, double* time_out);
char* generate_obligation_hash(ProofObligation* obligation);

// Contract translation
SMTExpression* contract_to_smt_expr(ContractExpression* contract, struct Type* context);

#endif // PROOF_GENERATION_H
