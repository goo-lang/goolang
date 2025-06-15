#ifndef PANIC_FREE_H
#define PANIC_FREE_H

#include "ast.h"
#include "types.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct BoundsVerifier BoundsVerifier;
typedef struct BoundsConstraint BoundsConstraint;
typedef struct BoundsProof BoundsProof;
typedef struct SymbolicExpression SymbolicExpression;
typedef struct VerificationContext VerificationContext;

// Bounds verification modes
typedef enum {
    BOUNDS_CHECK_RUNTIME,       // Traditional runtime bounds checking
    BOUNDS_CHECK_PROVE_SAFE,    // Mathematically prove safety at compile time
    BOUNDS_CHECK_OPTIMIZE_OUT,  // Eliminate checks when proven safe
    BOUNDS_CHECK_HYBRID,        // Combine compile-time and runtime checking
    BOUNDS_CHECK_DISABLED       // No bounds checking (unsafe)
} BoundsCheckMode;

// Constraint types for bounds verification
typedef enum {
    CONSTRAINT_LESS_THAN,       // index < bound
    CONSTRAINT_LESS_EQUAL,      // index <= bound
    CONSTRAINT_GREATER_THAN,    // index > bound
    CONSTRAINT_GREATER_EQUAL,   // index >= bound
    CONSTRAINT_EQUAL,           // index == bound
    CONSTRAINT_NOT_EQUAL,       // index != bound
    CONSTRAINT_AND,             // constraint1 && constraint2
    CONSTRAINT_OR,              // constraint1 || constraint2
    CONSTRAINT_NOT,             // !constraint
    CONSTRAINT_IMPLIES          // constraint1 => constraint2
} ConstraintType;

// Symbolic expression types for mathematical modeling
typedef enum {
    SYMBOLIC_CONSTANT,          // Literal constant value
    SYMBOLIC_VARIABLE,          // Variable reference
    SYMBOLIC_BINARY_OP,         // Binary operation (+, -, *, /, %)
    SYMBOLIC_UNARY_OP,          // Unary operation (-, !)
    SYMBOLIC_FUNCTION_CALL,     // Function call (len, cap, etc.)
    SYMBOLIC_ARRAY_LENGTH,      // Array length expression
    SYMBOLIC_CONDITIONAL        // Conditional expression (a ? b : c)
} SymbolicExpressionType;

// Proof status for bounds verification
typedef enum {
    PROOF_STATUS_UNKNOWN,       // Cannot determine safety
    PROOF_STATUS_SAFE,          // Mathematically proven safe
    PROOF_STATUS_UNSAFE,        // Proven to be potentially unsafe
    PROOF_STATUS_CONDITIONAL,   // Safe under certain conditions
    PROOF_STATUS_TIMEOUT,       // Proof generation timed out
    PROOF_STATUS_ERROR          // Error during proof generation
} ProofStatus;

// Symbolic expression for mathematical modeling
struct SymbolicExpression {
    SymbolicExpressionType type;
    
    union {
        // Constant value
        struct {
            int64_t value;
        } constant;
        
        // Variable reference
        struct {
            char* name;
            Type* type;
        } variable;
        
        // Binary operation
        struct {
            SymbolicExpression* left;
            SymbolicExpression* right;
            TokenType operator;  // +, -, *, /, %, <, >, <=, >=, ==, !=
        } binary_op;
        
        // Unary operation
        struct {
            SymbolicExpression* operand;
            TokenType operator;  // -, !
        } unary_op;
        
        // Function call
        struct {
            char* function_name;
            SymbolicExpression** arguments;
            size_t arg_count;
        } function_call;
        
        // Array length
        struct {
            SymbolicExpression* array_expr;
        } array_length;
        
        // Conditional expression
        struct {
            SymbolicExpression* condition;
            SymbolicExpression* true_expr;
            SymbolicExpression* false_expr;
        } conditional;
    } data;
    
    // Metadata
    Position source_pos;
    Type* result_type;
};

// Bounds constraint for verification
struct BoundsConstraint {
    ConstraintType type;
    
    union {
        // Simple constraints (comparison operations)
        struct {
            SymbolicExpression* left;
            SymbolicExpression* right;
        } comparison;
        
        // Compound constraints (logical operations)
        struct {
            BoundsConstraint* left;
            BoundsConstraint* right;
        } compound;
        
        // Unary constraint (negation)
        struct {
            BoundsConstraint* operand;
        } unary;
    } data;
    
    // Additional metadata
    int is_assumption;      // True if this is an assumed constraint
    double confidence;      // Confidence level (0.0 - 1.0)
    Position source_pos;    // Source location
};

// Bounds proof for verification results
struct BoundsProof {
    ProofStatus status;
    ASTNode* target_node;           // The array access being proven
    SymbolicExpression* index_expr; // Index expression
    SymbolicExpression* bound_expr; // Bound expression
    
    // Proof details
    BoundsConstraint** assumptions; // Required assumptions
    size_t assumption_count;
    BoundsConstraint** conclusions; // Proven conclusions
    size_t conclusion_count;
    
    // Proof steps (for debugging/explanation)
    char** proof_steps;
    size_t step_count;
    
    // Optimization information
    int can_eliminate_check;        // Can runtime check be eliminated?
    int requires_runtime_check;     // Must keep runtime check?
    char* optimization_note;        // Explanation of optimization
    
    // Metadata
    double proof_time;              // Time to generate proof (seconds)
    size_t proof_complexity;       // Complexity metric
    Position source_pos;
};

// Verification context for bounds checking
struct VerificationContext {
    TypeChecker* type_checker;
    
    // Current function being analyzed
    ASTNode* current_function;
    Scope* current_scope;
    
    // Accumulated constraints from control flow
    BoundsConstraint** active_constraints;
    size_t constraint_count;
    size_t constraint_capacity;
    
    // Symbol table for symbolic expressions
    SymbolicExpression** symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    
    // Configuration
    BoundsCheckMode default_mode;
    int enable_proof_optimization;
    int enable_loop_analysis;
    int max_proof_complexity;
    double proof_timeout;
    
    // Statistics
    size_t proofs_attempted;
    size_t proofs_successful;
    size_t checks_eliminated;
    size_t checks_kept;
    
    // Error tracking
    int error_count;
    int warning_count;
};

// Main bounds verifier
struct BoundsVerifier {
    VerificationContext* context;
    
    // Verification configuration
    int enable_smt_solver;          // Use SMT solver for complex proofs
    int enable_invariant_inference; // Automatically infer loop invariants
    int enable_path_sensitivity;    // Track constraints through branches
    int max_unroll_depth;          // Maximum loop unrolling for analysis
    
    // Proof cache for performance
    BoundsProof** proof_cache;
    size_t cache_size;
    size_t cache_capacity;
    
    // Statistics
    size_t total_array_accesses;
    size_t proven_safe_accesses;
    size_t eliminated_checks;
    size_t complex_proofs;
    double total_proof_time;
    
    // Error tracking
    int error_count;
    int warning_count;
};

// =============================================================================
// Bounds Verifier Management
// =============================================================================

// Create and destroy bounds verifier
BoundsVerifier* bounds_verifier_new(TypeChecker* type_checker);
void bounds_verifier_free(BoundsVerifier* verifier);

// Verification context management
VerificationContext* verification_context_new(TypeChecker* type_checker);
void verification_context_free(VerificationContext* context);

// Configuration
void bounds_verifier_set_mode(BoundsVerifier* verifier, BoundsCheckMode mode);
void bounds_verifier_enable_feature(BoundsVerifier* verifier, const char* feature, int enable);

// =============================================================================
// Symbolic Expression Management
// =============================================================================

// Create symbolic expressions
SymbolicExpression* symbolic_expression_new(SymbolicExpressionType type);
void symbolic_expression_free(SymbolicExpression* expr);
SymbolicExpression* symbolic_expression_copy(const SymbolicExpression* expr);

// Create specific expression types
SymbolicExpression* symbolic_constant(int64_t value);
SymbolicExpression* symbolic_variable(const char* name, Type* type);
SymbolicExpression* symbolic_binary_op(SymbolicExpression* left, TokenType op, SymbolicExpression* right);
SymbolicExpression* symbolic_unary_op(TokenType op, SymbolicExpression* operand);
SymbolicExpression* symbolic_function_call(const char* func_name, SymbolicExpression** args, size_t arg_count);
SymbolicExpression* symbolic_array_length(SymbolicExpression* array_expr);

// Expression operations
SymbolicExpression* symbolic_expression_simplify(SymbolicExpression* expr);
int symbolic_expression_equals(const SymbolicExpression* a, const SymbolicExpression* b);
char* symbolic_expression_to_string(const SymbolicExpression* expr);

// Convert AST expressions to symbolic expressions
SymbolicExpression* ast_to_symbolic_expression(ASTNode* ast_expr, VerificationContext* context);

// =============================================================================
// Bounds Constraint Management
// =============================================================================

// Create bounds constraints
BoundsConstraint* bounds_constraint_new(ConstraintType type);
void bounds_constraint_free(BoundsConstraint* constraint);
BoundsConstraint* bounds_constraint_copy(const BoundsConstraint* constraint);

// Create specific constraint types
BoundsConstraint* bounds_constraint_comparison(ConstraintType type, SymbolicExpression* left, SymbolicExpression* right);
BoundsConstraint* bounds_constraint_and(BoundsConstraint* left, BoundsConstraint* right);
BoundsConstraint* bounds_constraint_or(BoundsConstraint* left, BoundsConstraint* right);
BoundsConstraint* bounds_constraint_not(BoundsConstraint* operand);

// Constraint operations
BoundsConstraint* bounds_constraint_simplify(BoundsConstraint* constraint);
int bounds_constraint_implies(const BoundsConstraint* premise, const BoundsConstraint* conclusion);
char* bounds_constraint_to_string(const BoundsConstraint* constraint);

// =============================================================================
// Bounds Verification Core
// =============================================================================

// Main verification functions
BoundsProof* verify_array_access(BoundsVerifier* verifier, ASTNode* array_access);
BoundsProof* verify_bounds_safety(BoundsVerifier* verifier, SymbolicExpression* index, SymbolicExpression* bound);

// Proof generation
BoundsProof* generate_bounds_proof(VerificationContext* context, ASTNode* array_access);
ProofStatus determine_proof_status(VerificationContext* context, BoundsConstraint* goal);

// Constraint collection and analysis
int collect_constraints_from_control_flow(VerificationContext* context, ASTNode* node);
int add_constraint_from_condition(VerificationContext* context, ASTNode* condition, int is_true_branch);

// Loop analysis
int analyze_loop_bounds(VerificationContext* context, ASTNode* loop_node);
int infer_loop_invariants(VerificationContext* context, ASTNode* loop_node);

// =============================================================================
// Analysis and Optimization
// =============================================================================

// Program analysis
int analyze_function_bounds(BoundsVerifier* verifier, ASTNode* function);
int analyze_statement_bounds(BoundsVerifier* verifier, ASTNode* statement);
int analyze_expression_bounds(BoundsVerifier* verifier, ASTNode* expression);

// Optimization
int optimize_bounds_checks(BoundsVerifier* verifier, ASTNode* node);
int can_eliminate_bounds_check(BoundsProof* proof);
int should_keep_runtime_check(BoundsProof* proof);

// Path-sensitive analysis
int analyze_conditional_bounds(VerificationContext* context, ASTNode* if_stmt);
int merge_path_constraints(VerificationContext* context, BoundsConstraint** path1, size_t count1,
                          BoundsConstraint** path2, size_t count2);

// =============================================================================
// Integration and Code Generation
// =============================================================================

// Integration with type checker
int integrate_bounds_verifier_with_type_checker(TypeChecker* type_checker, BoundsVerifier* verifier);

// Annotation processing
BoundsCheckMode parse_bounds_check_annotation(ASTNode* annotation);
int process_comptime_assert(VerificationContext* context, ASTNode* assert_stmt);

// Code generation integration
int apply_bounds_optimizations_to_codegen(BoundsVerifier* verifier, ASTNode* program);
char* generate_runtime_check_code(BoundsProof* proof);
char* generate_optimized_access_code(BoundsProof* proof);

// =============================================================================
// Utilities and Debugging
// =============================================================================

// Proof management
BoundsProof* bounds_proof_new(ASTNode* target_node);
void bounds_proof_free(BoundsProof* proof);
void bounds_proof_add_step(BoundsProof* proof, const char* step_description);

// Statistics and reporting
void bounds_verifier_print_statistics(BoundsVerifier* verifier);
void bounds_verifier_print_proof(BoundsProof* proof);
void bounds_verifier_dump_constraints(VerificationContext* context, const char* filename);

// Error reporting
void bounds_verifier_error(VerificationContext* context, Position pos, const char* format, ...);
void bounds_verifier_warning(VerificationContext* context, Position pos, const char* format, ...);

// Utility functions
const char* bounds_check_mode_to_string(BoundsCheckMode mode);
const char* proof_status_to_string(ProofStatus status);
const char* constraint_type_to_string(ConstraintType type);

#endif // PANIC_FREE_H