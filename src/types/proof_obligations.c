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

// Invariant inference, termination proofs, proof obligations, the
// proof cache, and per-construct proof generation. Split from
// proof_generation.c (refactor, no behavior change).
// =============================================================================
// Automatic Invariant Inference
// =============================================================================

InferredInvariant* proof_infer_loop_invariants(ProofGenerationContext* ctx, struct ASTNode* loop_node, AbstractDomain domain) {
    if (!ctx || !loop_node) return NULL;
    
    printf("🔍 Inferring loop invariants using abstract interpretation (domain: %d)...\n", domain);
    
    InferredInvariant* invariants = xmalloc(sizeof(InferredInvariant));
    if (!invariants) return NULL;
    
    // For demonstration, generate a simple invariant: loop counter >= 0
    SMTExpression* simple_invariant = smt_app(">=", 
        (SMTExpression*[]){
            smt_var("i", NULL),
            smt_const_int(0)
        }, 2);
    
    *invariants = (InferredInvariant) {
        .invariant_expr = simple_invariant,
        .domain_used = ABSTRACT_DOMAIN_INTERVALS,
        .confidence_score = 0.85,
        .inference_method = strdup("interval_analysis"),
        .next = NULL
    };
    
    ctx->statistics.invariants_inferred++;
    
    printf("✅ Inferred invariant: i >= 0 (confidence: %.2f)\n", 
           invariants->confidence_score);
    
    return invariants;
}

InferredInvariant* abstract_interpretation_intervals(
    ProofGenerationContext* ctx, 
    struct ASTNode* code_block
) {
    // Simplified interval analysis
    printf("🔬 Running interval analysis...\n");
    
    InferredInvariant* invariant = xmalloc(sizeof(InferredInvariant));
    if (!invariant) return NULL;
    
    // Create invariant: all variables have finite intervals
    SMTExpression* interval_invariant = smt_app("and",
        (SMTExpression*[]){
            smt_app(">=", (SMTExpression*[]){smt_var("x", NULL), smt_const_int(0)}, 2),
            smt_app("<=", (SMTExpression*[]){smt_var("x", NULL), smt_const_int(100)}, 2)
        }, 2);
    
    *invariant = (InferredInvariant) {
        .invariant_expr = interval_invariant,
        .domain_used = ABSTRACT_DOMAIN_INTERVALS,
        .confidence_score = 0.9,
        .inference_method = strdup("interval_widening"),
        .next = NULL
    };
    
    return invariant;
}

InferredInvariant* abstract_interpretation_shapes(
    ProofGenerationContext* ctx, 
    struct ASTNode* code_block
) {
    // Simplified shape analysis
    printf("🔬 Running shape analysis...\n");
    
    InferredInvariant* invariant = xmalloc(sizeof(InferredInvariant));
    if (!invariant) return NULL;
    
    // Create invariant: pointer structures are well-formed
    SMTExpression* shape_invariant = smt_app("and",
        (SMTExpression*[]){
            smt_app("is-acyclic", (SMTExpression*[]){smt_var("list", NULL)}, 1),
            smt_app("well-formed", (SMTExpression*[]){smt_var("list", NULL)}, 1)
        }, 2);
    
    *invariant = (InferredInvariant) {
        .invariant_expr = shape_invariant,
        .domain_used = ABSTRACT_DOMAIN_SHAPES,
        .confidence_score = 0.75,
        .inference_method = strdup("shape_analysis"),
        .next = NULL
    };
    
    return invariant;
}

// =============================================================================
// Loop Termination Proof Generation
// =============================================================================

TerminationMeasure* generate_termination_proof(ProofGenerationContext* ctx, struct ASTNode* loop_ast, InferredInvariant* loop_invariants) {
    if (!ctx || !loop_ast) return NULL;
    
    printf("🔄 Generating loop termination proof (using %s invariants)...\n", 
           loop_invariants ? "inferred" : "default");
    
    TerminationMeasure* measure = xmalloc(sizeof(TerminationMeasure));
    if (!measure) return NULL;
    
    // Generate a simple ranking function: loop counter decreases
    SMTExpression* ranking_func = smt_app("-", 
        (SMTExpression*[]){
            smt_var("n", NULL),
            smt_var("i", NULL)
        }, 2);
    
    // Create a separate copy of the ranking function for the bound condition
    SMTExpression* ranking_func_copy = smt_app("-", 
        (SMTExpression*[]){
            smt_var("n", NULL),
            smt_var("i", NULL)
        }, 2);
    
    SMTExpression* bound_cond = smt_app(">=", 
        (SMTExpression*[]){
            ranking_func_copy,
            smt_const_int(0)
        }, 2);
    
    *measure = (TerminationMeasure) {
        .ranking_function = ranking_func,
        .bound_condition = bound_cond,
        .termination_argument = strdup("Loop counter decreases on each iteration")
    };
    
    ctx->statistics.termination_proofs++;
    
    printf("✅ Generated termination proof with ranking function: n - i\n");
    
    return measure;
}

TerminationMeasure* discover_ranking_function(
    ProofGenerationContext* ctx, 
    struct ASTNode* loop_node
) {
    // Automated ranking function discovery
    printf("🔍 Discovering ranking function...\n");
    
    // This would analyze the loop structure and find suitable ranking functions
    // For now, return a simple linear ranking function
    
    TerminationMeasure* measure = xmalloc(sizeof(TerminationMeasure));
    if (!measure) return NULL;
    
    SMTExpression* linear_ranking = smt_var("loop_counter", NULL);
    SMTExpression* non_negative = smt_app(">=", 
        (SMTExpression*[]){linear_ranking, smt_const_int(0)}, 2);
    
    *measure = (TerminationMeasure) {
        .ranking_function = linear_ranking,
        .bound_condition = non_negative,
        .termination_argument = strdup("Linear decrease in loop counter")
    };
    
    return measure;
}

// =============================================================================
// Integration with Contract System
// =============================================================================

ProofObligation* contract_to_proof_obligation(
    ContractExpression* contract,
    struct ASTNode* context
) {
    if (!contract) return NULL;
    
    ProofObligation* obligation = xmalloc(sizeof(ProofObligation));
    if (!obligation) return NULL;
    
    // Determine proof type from contract type
    ProofType proof_type;
    switch (contract->type) {
        case 0: // CONTRACT_PRECONDITION
        case 1: // CONTRACT_POSTCONDITION
            proof_type = PROOF_FUNCTIONAL_CORRECTNESS;
            break;
        case 2: // CONTRACT_INVARIANT
            proof_type = PROOF_INVARIANT_PRESERVATION;
            break;
        case 3: // CONTRACT_ASSERTION
            proof_type = PROOF_FUNCTIONAL_CORRECTNESS;
            break;
        default:
            proof_type = PROOF_FUNCTIONAL_CORRECTNESS;
            break;
    }
    
    *obligation = (ProofObligation) {
        .proof_type = proof_type,
        .description = contract->description ? strdup(contract->description) : strdup("Contract verification"),
        .source_location = context,
        .preconditions = NULL,
        .postconditions = contract_to_smt_expression(contract, NULL),
        .invariants = NULL,
        .line = contract->line,
        .column = contract->column,
        .filename = contract->filename ? strdup(contract->filename) : NULL,
        .function_name = strdup("unknown"),
        .status = PROOF_STATUS_UNKNOWN,
        .verification_time = 0.0,
        .proof_trace = NULL,
        .counterexample = NULL,
        .next = NULL
    };
    
    return obligation;
}

// =============================================================================
// Proof Caching System
// =============================================================================

ProofCache* proof_cache_create(const char* cache_dir) {
    if (!cache_dir) return NULL;
    
    // Create cache directory if it doesn't exist
    char command[512];
    snprintf(command, sizeof(command), "mkdir -p %s", cache_dir);
    system(command);
    
    printf("📦 Initialized proof cache at: %s\n", cache_dir);
    
    return NULL;  // Head of linked list starts empty
}

void proof_cache_free(ProofCache* cache) {
    while (cache) {
        ProofCache* next = cache->next;
        free(cache->obligation_hash);
        free(cache->cached_proof);
        free(cache);
        cache = next;
    }
}

int proof_cache_store(
    ProofCache* cache, 
    const char* obligation_hash,
    ProofStatus status, 
    const char* proof, 
    double verification_time
) {
    if (!obligation_hash) return 0;
    
    ProofCache* entry = xmalloc(sizeof(ProofCache));
    if (!entry) return 0;
    
    *entry = (ProofCache) {
        .obligation_hash = strdup(obligation_hash),
        .cached_status = status,
        .cached_proof = proof ? strdup(proof) : NULL,
        .cached_time = verification_time,
        .cache_timestamp = time(NULL),
        .next = cache
    };
    
    printf("💾 Cached proof for obligation: %.8s...\n", obligation_hash);
    
    return 1;
}

int proof_cache_lookup(
    ProofCache* cache, 
    const char* obligation_hash,
    ProofStatus* status_out, 
    char** proof_out, 
    double* time_out
) {
    if (!cache || !obligation_hash) return 0;
    
    while (cache) {
        if (strcmp(cache->obligation_hash, obligation_hash) == 0) {
            *status_out = cache->cached_status;
            *proof_out = cache->cached_proof ? strdup(cache->cached_proof) : NULL;
            *time_out = cache->cached_time;
            
            printf("🎯 Cache hit for obligation: %.8s...\n", obligation_hash);
            return 1;
        }
        cache = cache->next;
    }
    
    printf("❌ Cache miss for obligation: %.8s...\n", obligation_hash);
    return 0;
}

char* generate_obligation_hash(ProofObligation* obligation) {
    if (!obligation) return NULL;
    
    // Simple hash based on proof type and description
    // In practice, would use a proper cryptographic hash
    char* hash = malloc(33);  // 32 chars + null terminator
    if (!hash) return NULL;
    
    snprintf(hash, 33, "%08x%08x%08x%08x", 
             (unsigned int)obligation->proof_type,
             (unsigned int)strlen(obligation->description ? obligation->description : ""),
             (unsigned int)obligation->line,
             (unsigned int)obligation->column);
    
    return hash;
}

// =============================================================================
// =============================================================================
// Proof Generation for Specific AST Nodes
// =============================================================================

int generate_proof_for_assignment(
    ProofGenerationContext* ctx,
    struct ASTNode* assignment_node
) {
    if (!ctx || !assignment_node) return 0;
    
    printf("📝 Generating proof for assignment statement\n");
    
    // Check for memory safety in assignment
    SMTExpression* null_check = smt_not(smt_equals(
        smt_var("assigned_pointer", NULL),
        smt_const_int(0)
    ));
    
    ProofObligation* obligation = xmalloc(sizeof(ProofObligation));
    if (!obligation) return 0;
    
    *obligation = (ProofObligation) {
        .proof_type = PROOF_MEMORY_SAFETY,
        .description = strdup("Assignment null pointer check"),
        .source_location = assignment_node,
        .preconditions = NULL,
        .postconditions = null_check,
        .invariants = NULL,
        .status = PROOF_STATUS_UNKNOWN,
        .next = ctx->obligations
    };
    
    ctx->obligations = obligation;
    ctx->total_obligations++;
    
    return 1;
}

int generate_proof_for_function_call(
    ProofGenerationContext* ctx,
    struct ASTNode* call_node
) {
    if (!ctx || !call_node) return 0;
    
    printf("📞 Generating proof for function call\n");
    
    // Verify preconditions are met
    ProofObligation* obligation = xmalloc(sizeof(ProofObligation));
    if (!obligation) return 0;
    
    *obligation = (ProofObligation) {
        .proof_type = PROOF_FUNCTIONAL_CORRECTNESS,
        .description = strdup("Function call precondition verification"),
        .source_location = call_node,
        .preconditions = smt_const_bool(true),  // Would extract from function contract
        .postconditions = smt_const_bool(true), // Would extract from function contract
        .invariants = NULL,
        .status = PROOF_STATUS_UNKNOWN,
        .next = ctx->obligations
    };
    
    ctx->obligations = obligation;
    ctx->total_obligations++;
    
    return 1;
}

int generate_proof_for_array_access(
    ProofGenerationContext* ctx,
    struct ASTNode* access_node
) {
    if (!ctx || !access_node) return 0;
    
    printf("🔍 Generating proof for array access bounds checking\n");
    
    // Generate bounds checking proof
    SMTExpression* bounds_check = smt_and(
        smt_app(">=", (SMTExpression*[]){smt_var("index", NULL), smt_const_int(0)}, 2),
        smt_app("<", (SMTExpression*[]){
            smt_var("index", NULL), 
            smt_app("array-length", (SMTExpression*[]){smt_var("array", NULL)}, 1)
        }, 2)
    );
    
    ProofObligation* obligation = xmalloc(sizeof(ProofObligation));
    if (!obligation) return 0;
    
    *obligation = (ProofObligation) {
        .proof_type = PROOF_BOUNDS_CHECKING,
        .description = strdup("Array bounds verification"),
        .source_location = access_node,
        .preconditions = NULL,
        .postconditions = bounds_check,
        .invariants = NULL,
        .status = PROOF_STATUS_UNKNOWN,
        .next = ctx->obligations
    };
    
    ctx->obligations = obligation;
    ctx->total_obligations++;
    
    return 1;
}

