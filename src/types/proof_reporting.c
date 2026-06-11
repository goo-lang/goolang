#include "../../include/proof_generation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#include <unistd.h>    // For mkstemp, write, close, unlink
#include <sys/stat.h>  // For mkdir

// C23 compatibility checks
_Static_assert(sizeof(ProofType) == sizeof(int), "ProofType should be int size");
_Static_assert(sizeof(SMTSolver) == sizeof(int), "SMTSolver should be int size");

// Statistics, report generation, and error formatting for the proof
// system. Split from proof_generation.c (refactor, no behavior change).
// =============================================================================
// Statistics and Reporting Functions
// =============================================================================

void print_proof_generation_statistics(ProofGenerationContext* ctx) {
    if (!ctx) return;
    
    printf("\n📊 Proof Generation Statistics\n");
    printf("===============================\n");
    printf("Total obligations: %zu\n", ctx->total_obligations);
    printf("Verified obligations: %zu\n", ctx->verified_obligations);
    printf("Failed obligations: %zu\n", ctx->failed_obligations);
    printf("Total verification time: %.3f seconds\n", ctx->statistics.total_verification_time);
    printf("SMT queries generated: %zu\n", ctx->statistics.smt_queries_generated);
    printf("Invariants inferred: %zu\n", ctx->statistics.invariants_inferred);
    printf("Termination proofs: %zu\n", ctx->statistics.termination_proofs);
    printf("Memory safety proofs: %zu\n", ctx->statistics.memory_safety_proofs);
    
    if (ctx->use_proof_caching) {
        size_t total_cache_requests = ctx->cache_hit_count + ctx->cache_miss_count;
        double hit_rate = total_cache_requests > 0 ? 
                         (double)ctx->cache_hit_count / total_cache_requests * 100.0 : 0.0;
        printf("Cache hit rate: %.1f%% (%zu hits, %zu misses)\n", 
               hit_rate, ctx->cache_hit_count, ctx->cache_miss_count);
    }
    
    printf("SMT solver: %s\n", 
           ctx->solver_backend == SMT_SOLVER_Z3 ? "Z3" :
           ctx->solver_backend == SMT_SOLVER_CVC5 ? "CVC5" :
           ctx->solver_backend == SMT_SOLVER_YICES ? "Yices" : "Unknown");
    printf("===============================\n\n");
}

ProofReport* generate_proof_summary(ProofGenerationContext* ctx) {
    if (!ctx) return NULL;
    
    ProofReport* report = malloc(sizeof(ProofReport));
    if (!report) return NULL;
    
    *report = (ProofReport) {
        .total_functions_analyzed = 1,  // Would track properly in real implementation
        .total_proofs_generated = ctx->total_obligations,
        .memory_safety_proofs = ctx->statistics.memory_safety_proofs,
        .termination_proofs = ctx->statistics.termination_proofs,
        .functional_correctness_proofs = ctx->verified_obligations,
        .average_verification_time = ctx->total_obligations > 0 ? 
                                    ctx->statistics.total_verification_time / ctx->total_obligations : 0.0,
        .cache_hit_rate_percent = 0,  // Would calculate from cache statistics
        .detailed_statistics = NULL
    };
    
    // Generate detailed statistics string
    char* stats = malloc(1024);
    if (stats) {
        snprintf(stats, 1024,
                "Detailed Proof Generation Report:\n"
                "- Total SMT queries: %zu\n"
                "- Average query time: %.3f ms\n"
                "- Invariants discovered: %zu\n"
                "- Termination measures found: %zu\n",
                ctx->statistics.smt_queries_generated,
                ctx->statistics.smt_queries_generated > 0 ? 
                    ctx->statistics.total_verification_time * 1000.0 / ctx->statistics.smt_queries_generated : 0.0,
                ctx->statistics.invariants_inferred,
                ctx->statistics.termination_proofs);
        report->detailed_statistics = stats;
    }
    
    return report;
}

void generate_proof_report(ProofGenerationContext* ctx, const char* output_file) {
    if (!ctx || !output_file) return;
    
    FILE* file = fopen(output_file, "w");
    if (!file) return;
    
    fprintf(file, "# Proof Generation Report\n\n");
    fprintf(file, "## Summary\n");
    fprintf(file, "- Total proof obligations: %zu\n", ctx->total_obligations);
    fprintf(file, "- Successfully verified: %zu\n", ctx->verified_obligations);
    fprintf(file, "- Failed verification: %zu\n", ctx->failed_obligations);
    fprintf(file, "- Total time: %.3f seconds\n\n", ctx->statistics.total_verification_time);
    
    fprintf(file, "## Proof Types\n");
    fprintf(file, "- Memory safety proofs: %zu\n", ctx->statistics.memory_safety_proofs);
    fprintf(file, "- Termination proofs: %zu\n", ctx->statistics.termination_proofs);
    fprintf(file, "- Invariants inferred: %zu\n", ctx->statistics.invariants_inferred);
    fprintf(file, "- SMT queries generated: %zu\n\n", ctx->statistics.smt_queries_generated);
    
    fprintf(file, "## Solver Configuration\n");
    fprintf(file, "- Backend: %s\n", 
            ctx->solver_backend == SMT_SOLVER_Z3 ? "Z3" : "Other");
    fprintf(file, "- Timeout: %d seconds\n", ctx->solver_timeout_seconds);
    fprintf(file, "- Caching enabled: %s\n", ctx->use_proof_caching ? "Yes" : "No");
    
    fclose(file);
    printf("📄 Proof report generated: %s\n", output_file);
}

// =============================================================================
// Error Handling and Reporting
// =============================================================================

void report_proof_generation_error(ProofGenerationError* error) {
    if (!error) return;
    
    printf("❌ Proof Generation Error:\n");
    printf("   Type: ");
    switch (error->error_type) {
        case PROOF_ERROR_SMT_SOLVER:
            printf("SMT Solver Error\n");
            break;
        case PROOF_ERROR_TIMEOUT:
            printf("Verification Timeout\n");
            break;
        case PROOF_ERROR_UNSUPPORTED:
            printf("Unsupported Feature\n");
            break;
        case PROOF_ERROR_INVALID_INPUT:
            printf("Invalid Input\n");
            break;
        case PROOF_ERROR_CACHE:
            printf("Cache Error\n");
            break;
        case PROOF_ERROR_MEMORY:
            printf("Memory Error\n");
            break;
        case PROOF_ERROR_INVARIANT:
            printf("Invariant Inference Failed\n");
            break;
        default:
            printf("Unknown Error\n");
            break;
    }
    
    if (error->error_message) {
        printf("   Message: %s\n", error->error_message);
    }
    if (error->error_location) {
        printf("   Location: %s\n", error->error_location);
    }
    if (error->failed_obligation) {
        printf("   Failed obligation: %s\n", 
               error->failed_obligation->description ? 
               error->failed_obligation->description : "Unknown");
    }
}

char* format_proof_failure_message(ProofObligation* obligation, const char* reason) {
    if (!obligation) return NULL;
    
    size_t msg_len = 256;
    if (obligation->description) msg_len += strlen(obligation->description);
    if (reason) msg_len += strlen(reason);
    
    char* message = malloc(msg_len);
    if (!message) return NULL;
    
    snprintf(message, msg_len,
             "Proof verification failed for %s at %s:%d:%d. Reason: %s",
             obligation->description ? obligation->description : "unknown obligation",
             obligation->filename ? obligation->filename : "unknown",
             obligation->line,
             obligation->column,
             reason ? reason : "unknown error");
    
    return message;
}

// =============================================================================
// Integration Functions
// =============================================================================

int integrate_contracts_with_proofs(
    ProofGenerationContext* proof_ctx,
    ContractContext* contract_ctx,
    DependentTypeContext* type_ctx
) {
    if (!proof_ctx || !contract_ctx) return 0;
    
    printf("🔗 Integrating contracts with proof generation...\n");
    
    int integrated = 0;
    
    // Process function contracts
    FunctionContract* func_contract = contract_ctx->function_contracts;
    while (func_contract) {
        // Generate proof obligations for preconditions
        ContractExpression* expr = func_contract->preconditions;
        while (expr) {
            ProofObligation* obligation = contract_to_proof_obligation(expr, NULL);
            if (obligation) {
                obligation->next = proof_ctx->obligations;
                proof_ctx->obligations = obligation;
                proof_ctx->total_obligations++;
                integrated++;
            }
            expr = expr->next;
        }
        
        // Generate proof obligations for postconditions
        expr = func_contract->postconditions;
        while (expr) {
            ProofObligation* obligation = contract_to_proof_obligation(expr, NULL);
            if (obligation) {
                obligation->next = proof_ctx->obligations;
                proof_ctx->obligations = obligation;
                proof_ctx->total_obligations++;
                integrated++;
            }
            expr = expr->next;
        }
        
        func_contract = func_contract->next;
    }
    
    printf("✅ Integrated %d contract obligations\n", integrated);
    return integrated;
}

int verify_contract_with_smt(
    ProofGenerationContext* ctx,
    ContractExpression* contract,
    SMTExpression** proof_out
) {
    if (!ctx || !contract || !proof_out) return 0;
    
    printf("🔍 Verifying contract with SMT solver...\n");
    
    // Convert contract to SMT expression
    SMTExpression* smt_contract = contract_to_smt_expr(contract, NULL);
    if (!smt_contract) return 0;
    
    // Check satisfiability
    SMTResult result = smt_check_satisfiability(ctx, smt_contract);
    
    *proof_out = smt_contract;
    
    switch (result) {
        case SMT_RESULT_UNSAT:
            printf("✅ Contract verified: unsatisfiable (proof valid)\n");
            return 1;
        case SMT_RESULT_SAT:
            printf("❌ Contract verification failed: satisfiable (counterexample exists)\n");
            return 0;
        case SMT_RESULT_TIMEOUT:
            printf("⏰ Contract verification timed out\n");
            return 0;
        default:
            printf("❓ Contract verification result unknown\n");
            return 0;
    }
}

// =============================================================================
// Utility Functions
// =============================================================================

bool is_smt_expression_valid(SMTExpression* expr) {
    if (!expr) return false;
    
    switch (expr->type) {
        case SMT_VAR:
            return expr->variable.name != NULL;
        case SMT_CONST:
            return true;  // Constants are always valid
        case SMT_APP:
            if (!expr->application.function_name) return false;
            for (size_t i = 0; i < expr->application.arg_count; i++) {
                if (!is_smt_expression_valid(expr->application.args[i])) {
                    return false;
                }
            }
            return true;
        case SMT_QUANTIFIER:
            return expr->quantifier.bound_vars != NULL &&
                   expr->quantifier.var_count > 0 &&
                   is_smt_expression_valid(expr->quantifier.body);
        default:
            return false;
    }
}

bool is_proof_obligation_satisfiable(ProofObligation* obligation) {
    if (!obligation) return false;
    
    // Check if the obligation has necessary components
    return obligation->description != NULL &&
           (obligation->preconditions != NULL || 
            obligation->postconditions != NULL ||
            obligation->invariants != NULL);
}

int estimate_proof_complexity(ProofObligation* obligation) {
    if (!obligation) return 0;
    
    int complexity = 1;  // Base complexity
    
    // Add complexity based on proof type
    switch (obligation->proof_type) {
        case PROOF_MEMORY_SAFETY:
            complexity += 2;
            break;
        case PROOF_TERMINATION:
            complexity += 3;
            break;
        case PROOF_FUNCTIONAL_CORRECTNESS:
            complexity += 2;
            break;
        case PROOF_INVARIANT_PRESERVATION:
            complexity += 4;
            break;
        default:
            complexity += 1;
            break;
    }
    
    // Add complexity based on SMT expressions
    if (obligation->preconditions) complexity++;
    if (obligation->postconditions) complexity++;
    if (obligation->invariants) complexity += 2;
    
    return complexity;
}

double calculate_confidence_score(InferredInvariant* invariant) {
    if (!invariant) return 0.0;
    
    double base_score = 0.5;  // Base confidence
    
    // Adjust based on inference domain
    switch (invariant->domain_used) {
        case ABSTRACT_DOMAIN_INTERVALS:
            base_score += 0.3;  // Intervals are fairly reliable
            break;
        case ABSTRACT_DOMAIN_SHAPES:
            base_score += 0.2;  // Shape analysis is more complex
            break;
        case ABSTRACT_DOMAIN_POLYHEDRA:
            base_score += 0.4;  // Polyhedra can be very precise
            break;
        default:
            base_score += 0.1;
            break;
    }
    
    // Ensure score is in [0,1] range
    return base_score > 1.0 ? 1.0 : (base_score < 0.0 ? 0.0 : base_score);
}

// =============================================================================
// Configuration Management
// =============================================================================

ProofGenerationConfig* proof_generation_config_default(void) {
    ProofGenerationConfig* config = malloc(sizeof(ProofGenerationConfig));
    if (!config) return NULL;
    
    *config = (ProofGenerationConfig) {
        .preferred_solver = SMT_SOLVER_Z3,
        .default_timeout = 30,
        .enable_proof_caching = true,
        
        .inference_domains = NULL,
        .domain_count = 0,
        .max_widening_iterations = 10,
        
        .enable_parallel_verification = false,
        .max_worker_threads = 4,
        .memory_limit_mb = 1024,
        
        .generate_detailed_proofs = true,
        .generate_counterexamples = true,
        .output_directory = strdup("./proof_output")
    };
    
    // Set up default domains
    config->inference_domains = malloc(3 * sizeof(AbstractDomain));
    if (config->inference_domains) {
        config->inference_domains[0] = ABSTRACT_DOMAIN_INTERVALS;
        config->inference_domains[1] = ABSTRACT_DOMAIN_SIGNS;
        config->inference_domains[2] = ABSTRACT_DOMAIN_POINTS_TO;
        config->domain_count = 3;
    }
    
    return config;
}

int proof_generation_config_load_from_file(const char* config_file, ProofGenerationConfig** config) {
    if (!config_file || !config) return 0;
    
    printf("📁 Loading proof generation config from: %s\n", config_file);
    
    // For now, return default config
    // In real implementation, would parse JSON/TOML config file
    *config = proof_generation_config_default();
    
    return *config != NULL ? 1 : 0;
}

void proof_generation_config_free(ProofGenerationConfig* config) {
    if (!config) return;
    
    free(config->inference_domains);
    free(config->output_directory);
    free(config);
}

// =============================================================================
// Memory Management Functions
// =============================================================================

void proof_obligation_free(ProofObligation* obligation) {
    while (obligation) {
        ProofObligation* next = obligation->next;
        
        free(obligation->description);
        free(obligation->filename);
        free(obligation->function_name);
        free(obligation->proof_trace);
        free(obligation->counterexample);
        
        smt_expression_free(obligation->preconditions);
        smt_expression_free(obligation->postconditions);
        smt_expression_free(obligation->invariants);
        
        free(obligation);
        obligation = next;
    }
}

void memory_safety_proof_free(MemorySafetyProof* proof) {
    if (!proof) return;
    
    smt_expression_free(proof->memory_model);
    
    if (proof->unsafe_operations) {
        for (size_t i = 0; i < proof->unsafe_count; i++) {
            free(proof->unsafe_operations[i]);
        }
        free(proof->unsafe_operations);
    }
    
    free(proof);
}

void inferred_invariant_free(InferredInvariant* invariant) {
    while (invariant) {
        InferredInvariant* next = invariant->next;
        
        smt_expression_free(invariant->invariant_expr);
        free(invariant->inference_method);
        
        free(invariant);
        invariant = next;
    }
}

void termination_measure_free(TerminationMeasure* measure) {
    if (!measure) return;
    
    smt_expression_free(measure->ranking_function);
    smt_expression_free(measure->bound_condition);
    free(measure->termination_argument);
    
    free(measure);
}

// =============================================================================
// Contract to SMT Translation Helper Functions
// =============================================================================

SMTExpression* contract_to_smt_expr(ContractExpression* contract, struct Type* context) {
    if (!contract) return NULL;
    (void)context;  // Suppress unused parameter warning
    
    // For now, create a simple boolean variable representing the contract
    // In a full implementation, we would traverse the contract->condition AST node
    // and convert it to proper SMT expressions
    
    switch (contract->type) {
        case 0: // CONTRACT_PRECONDITION
            return smt_var("precondition_holds", NULL);
        case 1: // CONTRACT_POSTCONDITION
            return smt_var("postcondition_holds", NULL);
        case 2: // CONTRACT_INVARIANT
            return smt_var("invariant_holds", NULL);
        case 3: // CONTRACT_ASSERTION
            return smt_var("assertion_holds", NULL);
        case 4: // CONTRACT_ASSUMPTION
            return smt_const_bool(true);  // Assumptions are always true
        default:
            return smt_const_bool(true);
    }
}

// Add the missing contract_to_smt_expression wrapper function
SMTExpression* contract_to_smt_expression(ContractExpression* contract, DependentTypeContext* type_ctx) {
    (void)type_ctx;  // Suppress unused parameter warning
    return contract_to_smt_expr(contract, NULL);
}
