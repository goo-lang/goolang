#ifndef CONTRACTS_H
#define CONTRACTS_H

#include "ccomp_shim.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ast.h"
#include "types.h"
#include "dependent_types.h"

// C23 features we'll use
_Static_assert(sizeof(bool) == 1, "bool should be 1 byte");
#ifdef __STDC_VERSION__
// Temporarily disabled for debugging
// _Static_assert(__STDC_VERSION__ >= 202311L, "C23 required");
#endif

// Contract types using C23 enum features
typedef enum GOO_ENUM_U8 {
    CONTRACT_PRECONDITION = 0,    // requires clause
    CONTRACT_POSTCONDITION,       // ensures clause
    CONTRACT_INVARIANT,           // loop invariant
    CONTRACT_ASSERTION,           // assert statement
    CONTRACT_ASSUMPTION,          // assume statement (for optimization)
    CONTRACT_TYPE_COUNT
} ContractType;

// Contract expression structure using C23 features
typedef struct ContractExpression {
    ContractType type;
    struct ASTNode* condition;           // Boolean expression to verify
    struct ASTNode* message;             // Optional error message
    
    // Source location info using anonymous struct (C23)
    struct {
        int line;
        int column;
        char* filename;
    };
    
    char* description;                   // Human-readable description
    bool is_compile_time;                // Can be verified at compile time
    struct ContractExpression* next;
} ContractExpression;

// Function contract structure
typedef struct FunctionContract {
    char* function_name;
    ContractExpression* preconditions;   // requires clauses
    ContractExpression* postconditions;  // ensures clauses
    ContractExpression* invariants;      // general invariants
    struct Type* return_type;            // For postcondition analysis
    struct ASTNode** parameters;        // For precondition analysis
    int parameter_count;
    struct FunctionContract* next;
} FunctionContract;

// Loop contract structure
typedef struct LoopContract {
    struct ASTNode* loop_node;           // The loop AST node
    ContractExpression* invariants;      // Loop invariants
    struct ASTNode* variant;             // Termination measure
    struct LoopContract* next;
} LoopContract;

// Contract verification context
typedef struct ContractContext {
    FunctionContract* function_contracts;
    LoopContract* loop_contracts;
    ContractExpression* global_invariants;
    int verification_enabled;            // Runtime vs compile-time
    int optimization_level;              // How aggressive to optimize
} ContractContext;

// Contract verification result
typedef enum {
    CONTRACT_VERIFIED,       // Statically verified at compile time
    CONTRACT_RUNTIME_CHECK,  // Needs runtime verification
    CONTRACT_VIOLATED,       // Statically determined to be false
    CONTRACT_UNKNOWN         // Cannot determine statically
} ContractVerificationResult;

typedef struct ContractVerificationInfo {
    ContractVerificationResult result;
    char* error_message;
    int can_optimize_away;       // If true, check can be removed
    struct ASTNode* witness;     // Counter-example if violated
} ContractVerificationInfo;

// =============================================================================
// Contract Creation Functions
// =============================================================================

// Create a new contract expression
ContractExpression* contract_expression_create(
    ContractType type,
    struct ASTNode* condition,
    const char* description
);

// Create function contract
FunctionContract* function_contract_create(const char* function_name);

// Create loop contract
LoopContract* loop_contract_create(struct ASTNode* loop_node);

// Create contract context
ContractContext* contract_context_create(void);

// =============================================================================
// Contract Management Functions
// =============================================================================

// Add precondition to function
int function_contract_add_precondition(
    FunctionContract* contract,
    struct ASTNode* condition,
    const char* description
);

// Add postcondition to function
int function_contract_add_postcondition(
    FunctionContract* contract,
    struct ASTNode* condition,
    const char* description
);

// Add invariant to loop
int loop_contract_add_invariant(
    LoopContract* contract,
    struct ASTNode* condition,
    const char* description
);

// Add contract to context
int contract_context_add_function(ContractContext* ctx, FunctionContract* contract);
int contract_context_add_loop(ContractContext* ctx, LoopContract* contract);

// =============================================================================
// Contract Verification Functions
// =============================================================================

// Verify a single contract expression
ContractVerificationInfo* verify_contract_expression(
    ContractExpression* expr,
    DependentTypeContext* type_ctx,
    struct ASTNode* current_scope
);

// Verify function contracts
int verify_function_contracts(
    FunctionContract* contract,
    struct ASTNode* function_body,
    DependentTypeContext* type_ctx
);

// Verify loop contracts
int verify_loop_contracts(
    LoopContract* contract,
    DependentTypeContext* type_ctx
);

// Verify all contracts in context
int verify_all_contracts(ContractContext* ctx, DependentTypeContext* type_ctx);

// =============================================================================
// Contract-based Optimization Functions
// =============================================================================

// Optimize based on contract information
int optimize_with_contracts(
    ContractContext* ctx,
    struct ASTNode* ast,
    DependentTypeContext* type_ctx
);

// Remove redundant bounds checks using contracts
int eliminate_bounds_checks(
    ContractContext* ctx,
    struct ASTNode* ast
);

// =============================================================================
// Integration with Dependent Types
// =============================================================================

// Create dependent type constraint from contract
DependentType* contract_to_dependent_constraint(
    ContractExpression* expr,
    struct Type* base_type
);

// Verify contract using dependent type system
int verify_contract_with_dependent_types(
    ContractExpression* expr,
    DependentType* dep_type,
    DependentTypeContext* type_ctx
);

// =============================================================================
// Error Handling and Reporting
// =============================================================================

// Contract violation error structure
typedef struct ContractViolation {
    ContractType contract_type;
    char* function_name;
    char* violation_message;
    int line;
    int column;
    struct ASTNode* violating_expression;
    struct ContractViolation* next;
} ContractViolation;

// Report contract violation  
void report_contract_violation(
    ContractViolation* violation
);

// Generate helpful error message for contract failure
char* generate_contract_error_message(
    ContractExpression* expr,
    struct ASTNode* context
);

// =============================================================================
// Memory Management
// =============================================================================

// Free contract expressions
void contract_expression_free(ContractExpression* expr);

// Free function contract
void function_contract_free(FunctionContract* contract);

// Free loop contract
void loop_contract_free(LoopContract* contract);

// Free contract context
void contract_context_free(ContractContext* ctx);

// Free verification info
void contract_verification_info_free(ContractVerificationInfo* info);

// Free contract violation
void contract_violation_free(ContractViolation* violation);

// =============================================================================
// Utility Functions
// =============================================================================

// Check if expression is a valid contract condition
int is_valid_contract_condition(struct ASTNode* expr);

// Simplify contract expression for optimization
struct ASTNode* simplify_contract_expression(
    struct ASTNode* expr,
    DependentTypeContext* type_ctx
);

// Convert contract to string representation
char* contract_to_string(ContractExpression* expr);

// Check contract compatibility with function signature
int is_contract_compatible(
    ContractExpression* expr,
    struct Type* function_type
);

#endif // CONTRACTS_H
