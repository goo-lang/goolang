#include "memory_safety.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// =============================================================================
// 19.3: Interprocedural Escape Analysis Implementation
// =============================================================================

// Helper function for string duplication
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// =============================================================================
// Escape Context Management
// =============================================================================

EscapeContext* escape_context_new(ASTNode* escape_site, ASTNode* target_function, 
                                 EscapeKind kind, size_t call_depth) {
    EscapeContext* context = xmalloc(sizeof(EscapeContext));
    if (!context) return NULL;
    
    context->escape_site = escape_site;
    context->target_function = target_function;
    context->escape_kind = kind;
    context->lifetime = LIFETIME_UNKNOWN;
    context->strategy = ALLOC_STRATEGY_DEFER;
    context->call_depth = call_depth;
    context->is_conditional = 0;
    context->escape_probability = 1.0;
    context->next = NULL;
    
    return context;
}

void escape_context_free(EscapeContext* context) {
    if (!context) return;
    free(context);
}

// =============================================================================
// Function Escape Information Management
// =============================================================================

FunctionEscapeInfo* function_escape_info_new(const char* function_name, ASTNode* function_node) {
    FunctionEscapeInfo* info = xmalloc(sizeof(FunctionEscapeInfo));
    if (!info) return NULL;
    
    info->function_name = str_dup(function_name);
    info->function_node = function_node;
    info->param_escape = NULL;
    info->param_alloc_strategy = NULL;
    info->param_count = 0;
    info->return_escape = ESCAPE_NONE;
    info->return_lifetime = LIFETIME_LOCAL;
    info->callees = NULL;
    info->callee_count = 0;
    info->callers = NULL;
    info->caller_count = 0;
    info->is_analyzed = 0;
    info->is_recursive = 0;
    info->has_side_effects = 0;
    info->complexity_score = 0;
    info->next = NULL;
    
    return info;
}

void function_escape_info_free(FunctionEscapeInfo* info) {
    if (!info) return;
    
    free(info->function_name);
    free(info->param_escape);
    free(info->param_alloc_strategy);
    free(info->callees);
    free(info->callers);
    free(info);
}

// =============================================================================
// Escape Analyzer Management
// =============================================================================

EscapeAnalyzer* escape_analyzer_new(ReferenceManager* reference_manager) {
    EscapeAnalyzer* analyzer = xmalloc(sizeof(EscapeAnalyzer));
    if (!analyzer) return NULL;
    
    analyzer->reference_manager = reference_manager;
    analyzer->functions = malloc(sizeof(FunctionEscapeInfo*) * 16);
    if (!analyzer->functions) {
        free(analyzer);
        return NULL;
    }
    
    analyzer->function_count = 0;
    analyzer->function_capacity = 16;
    
    analyzer->call_sites = malloc(sizeof(ASTNode*) * 32);
    if (!analyzer->call_sites) {
        free(analyzer->functions);
        free(analyzer);
        return NULL;
    }
    
    analyzer->call_site_count = 0;
    analyzer->call_site_capacity = 32;
    
    analyzer->escape_contexts = malloc(sizeof(EscapeContext*) * 32);
    if (!analyzer->escape_contexts) {
        free(analyzer->functions);
        free(analyzer->call_sites);
        free(analyzer);
        return NULL;
    }
    
    analyzer->context_count = 0;
    analyzer->context_capacity = 32;
    
    // Default configuration
    analyzer->enable_region_analysis = 1;
    analyzer->aggressive_stack_allocation = 1;
    analyzer->optimize_for_size = 0;
    analyzer->enable_probabilistic_analysis = 1;
    
    // Initialize statistics
    analyzer->functions_analyzed = 0;
    analyzer->escapes_detected = 0;
    analyzer->stack_allocations_recommended = 0;
    analyzer->heap_allocations_required = 0;
    analyzer->region_allocations_created = 0;
    analyzer->error_count = 0;
    analyzer->warning_count = 0;
    
    return analyzer;
}

void escape_analyzer_free(EscapeAnalyzer* analyzer) {
    if (!analyzer) return;
    
    // Free function infos
    for (size_t i = 0; i < analyzer->function_count; i++) {
        function_escape_info_free(analyzer->functions[i]);
    }
    free(analyzer->functions);
    
    // Free call sites
    free(analyzer->call_sites);
    
    // Free escape contexts
    for (size_t i = 0; i < analyzer->context_count; i++) {
        escape_context_free(analyzer->escape_contexts[i]);
    }
    free(analyzer->escape_contexts);
    
    free(analyzer);
}

// =============================================================================
// Function Registration and Call Graph Building
// =============================================================================

int escape_analyzer_register_function(EscapeAnalyzer* analyzer, const char* name, ASTNode* func_node) {
    if (!analyzer || !name || !func_node) return 0;
    
    // Check if we need to resize
    if (analyzer->function_count >= analyzer->function_capacity) {
        size_t new_capacity = analyzer->function_capacity * 2;
        FunctionEscapeInfo** new_functions = realloc(analyzer->functions,
            sizeof(FunctionEscapeInfo*) * new_capacity);
        if (!new_functions) return 0;
        
        analyzer->functions = new_functions;
        analyzer->function_capacity = new_capacity;
    }
    
    FunctionEscapeInfo* info = function_escape_info_new(name, func_node);
    if (!info) return 0;
    
    analyzer->functions[analyzer->function_count++] = info;
    return 1;
}

FunctionEscapeInfo* escape_analyzer_find_function(EscapeAnalyzer* analyzer, const char* name) {
    if (!analyzer || !name) return NULL;
    
    for (size_t i = 0; i < analyzer->function_count; i++) {
        if (strcmp(analyzer->functions[i]->function_name, name) == 0) {
            return analyzer->functions[i];
        }
    }
    return NULL;
}

int escape_analyzer_register_call_site(EscapeAnalyzer* analyzer, ASTNode* call_node) {
    if (!analyzer || !call_node) return 0;
    
    // Check if we need to resize
    if (analyzer->call_site_count >= analyzer->call_site_capacity) {
        size_t new_capacity = analyzer->call_site_capacity * 2;
        ASTNode** new_call_sites = realloc(analyzer->call_sites,
            sizeof(ASTNode*) * new_capacity);
        if (!new_call_sites) return 0;
        
        analyzer->call_sites = new_call_sites;
        analyzer->call_site_capacity = new_capacity;
    }
    
    analyzer->call_sites[analyzer->call_site_count++] = call_node;
    return 1;
}

// =============================================================================
// Escape Analysis Algorithms
// =============================================================================

EscapeKind analyze_expression_escape(EscapeAnalyzer* analyzer, ASTNode* expr, 
                                    FunctionEscapeInfo* current_function) {
    if (!analyzer || !expr) return ESCAPE_UNKNOWN;
    
    switch (expr->type) {
        case AST_IDENTIFIER: {
            // Check if this is a parameter that escapes
            for (size_t i = 0; i < current_function->param_count; i++) {
                // In a real implementation, we'd check parameter names
                // For now, assume local variables don't escape unless returned
                return ESCAPE_NONE;
            }
            return ESCAPE_NONE;
        }
        
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            
            // Register this call site
            escape_analyzer_register_call_site(analyzer, expr);
            
            // Look up the called function's escape information
            if (call->function) {
                if (call->function->type == AST_IDENTIFIER) {
                    IdentifierNode* func_name = (IdentifierNode*)call->function;
                    FunctionEscapeInfo* callee = escape_analyzer_find_function(analyzer, func_name->name);
                    
                    if (callee) {
                        // Return the escape kind of the called function's return value
                        return callee->return_escape;
                    } else {
                        // Unknown function - conservative assumption
                        return ESCAPE_FUNCTION;
                    }
                }
            }
            return ESCAPE_FUNCTION;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* bin_op = (BinaryExprNode*)expr;
            
            // For binary operations, take the maximum escape of operands
            EscapeKind left_escape = analyze_expression_escape(analyzer, bin_op->left, current_function);
            EscapeKind right_escape = analyze_expression_escape(analyzer, bin_op->right, current_function);
            
            return (left_escape > right_escape) ? left_escape : right_escape;
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary_op = (UnaryExprNode*)expr;
            return analyze_expression_escape(analyzer, unary_op->operand, current_function);
        }
        
        case AST_INDEX_EXPR: {
            IndexExprNode* index_expr = (IndexExprNode*)expr;
            
            // Array access inherits escape properties of the array
            EscapeKind array_escape = analyze_expression_escape(analyzer, index_expr->expr, current_function);
            analyze_expression_escape(analyzer, index_expr->index, current_function); // Analyze index too
            
            return array_escape;
        }
        
        case AST_SELECTOR_EXPR: {
            SelectorExprNode* selector_expr = (SelectorExprNode*)expr;
            
            // Field access inherits escape properties of the object
            return analyze_expression_escape(analyzer, selector_expr->expr, current_function);
        }
        
        case AST_LITERAL:
            // Literals don't escape unless assigned to something that does
            return ESCAPE_NONE;
            
        default:
            // Conservative assumption for unknown expression types
            return ESCAPE_UNKNOWN;
    }
}

int analyze_statement_escape(EscapeAnalyzer* analyzer, ASTNode* stmt, 
                            FunctionEscapeInfo* current_function) {
    if (!analyzer || !stmt || !current_function) return 0;
    
    switch (stmt->type) {
        case AST_VAR_DECL: {
            VarDeclNode* var_decl = (VarDeclNode*)stmt;
            
            if (var_decl->values) {
                EscapeKind escape = analyze_expression_escape(analyzer, var_decl->values, current_function);
                
                // Create escape context for this variable
                EscapeContext* context = escape_context_new(stmt, current_function->function_node, 
                                                          escape, 0);
                if (context) {
                    // Determine allocation strategy based on escape analysis
                    if (escape == ESCAPE_NONE) {
                        context->strategy = ALLOC_STRATEGY_STACK;
                        context->lifetime = LIFETIME_LOCAL;
                        analyzer->stack_allocations_recommended++;
                    } else if (escape == ESCAPE_FUNCTION) {
                        context->strategy = ALLOC_STRATEGY_HEAP;
                        context->lifetime = LIFETIME_RETURN;
                        analyzer->heap_allocations_required++;
                    } else {
                        context->strategy = ALLOC_STRATEGY_HEAP;
                        context->lifetime = LIFETIME_GLOBAL;
                        analyzer->heap_allocations_required++;
                    }
                    
                    // Add to analyzer's contexts
                    if (analyzer->context_count < analyzer->context_capacity) {
                        analyzer->escape_contexts[analyzer->context_count++] = context;
                        analyzer->escapes_detected++;
                    } else {
                        escape_context_free(context);
                    }
                }
            }
            break;
        }
        
        // Note: Assignment is typically handled as an expression statement
        // containing a binary expression with assignment operator
        
        case AST_RETURN_STMT: {
            ReturnStmtNode* ret = (ReturnStmtNode*)stmt;
            
            if (ret->values) {
                EscapeKind escape = analyze_expression_escape(analyzer, ret->values, current_function);
                
                // Update function's return escape information
                if (escape > current_function->return_escape) {
                    current_function->return_escape = escape;
                    
                    if (escape == ESCAPE_NONE) {
                        current_function->return_lifetime = LIFETIME_LOCAL;
                    } else {
                        current_function->return_lifetime = LIFETIME_RETURN;
                    }
                }
            }
            break;
        }
        
        case AST_IF_STMT: {
            IfStmtNode* if_node = (IfStmtNode*)stmt;
            
            // Analyze condition
            analyze_expression_escape(analyzer, if_node->condition, current_function);
            
            // Analyze then and else blocks
            if (if_node->then_stmt) {
                analyze_statement_escape(analyzer, if_node->then_stmt, current_function);
            }
            if (if_node->else_stmt) {
                analyze_statement_escape(analyzer, if_node->else_stmt, current_function);
            }
            break;
        }
        
        // Note: Go doesn't have while loops, only for loops
        
        case AST_FOR_STMT: {
            ForStmtNode* for_node = (ForStmtNode*)stmt;
            
            // Analyze all parts of the for loop
            if (for_node->init) {
                analyze_statement_escape(analyzer, for_node->init, current_function);
            }
            if (for_node->condition) {
                analyze_expression_escape(analyzer, for_node->condition, current_function);
            }
            if (for_node->post) {
                analyze_statement_escape(analyzer, for_node->post, current_function);
            }
            if (for_node->body) {
                analyze_statement_escape(analyzer, for_node->body, current_function);
            }
            break;
        }
        
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)stmt;
            
            // Analyze all statements in the block
            ASTNode* current_stmt = block->statements;
            while (current_stmt) {
                analyze_statement_escape(analyzer, current_stmt, current_function);
                current_stmt = current_stmt->next;
            }
            break;
        }
        
        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
            analyze_expression_escape(analyzer, expr_stmt->expr, current_function);
            break;
        }
        
        default:
            // Handle other statement types as needed
            break;
    }
    
    return 1;
}

int analyze_function_escape(EscapeAnalyzer* analyzer, FunctionEscapeInfo* func_info) {
    if (!analyzer || !func_info || func_info->is_analyzed) return 0;
    
    ASTNode* func_node = func_info->function_node;
    if (!func_node || func_node->type != AST_FUNC_DECL) return 0;
    
    FuncDeclNode* func_decl = (FuncDeclNode*)func_node;
    
    // Initialize parameter escape information
    if (func_decl->params) {
        // Count parameters by traversing linked list
        size_t param_count = 0;
        ASTNode* param = func_decl->params;
        while (param) {
            param_count++;
            param = param->next;
        }
        
        func_info->param_count = param_count;
        if (param_count > 0) {
            func_info->param_escape = malloc(sizeof(EscapeKind) * param_count);
            func_info->param_alloc_strategy = malloc(sizeof(AllocationStrategy) * param_count);
            
            if (func_info->param_escape && func_info->param_alloc_strategy) {
                // Initialize all parameters as non-escaping initially
                for (size_t i = 0; i < param_count; i++) {
                    func_info->param_escape[i] = ESCAPE_NONE;
                    func_info->param_alloc_strategy[i] = ALLOC_STRATEGY_STACK;
                }
            }
        }
    }
    
    // Analyze function body
    if (func_decl->body) {
        analyze_statement_escape(analyzer, func_decl->body, func_info);
    }
    
    // Calculate complexity score
    func_info->complexity_score = analyzer->context_count;
    
    // Mark as analyzed
    func_info->is_analyzed = 1;
    analyzer->functions_analyzed++;
    
    return 1;
}

// =============================================================================
// Interprocedural Analysis Driver
// =============================================================================

int escape_analyzer_analyze_program(EscapeAnalyzer* analyzer, ASTNode* program) {
    if (!analyzer || !program) return 0;
    
    // First pass: Collect all function definitions
    if (!collect_function_definitions(analyzer, program)) {
        return 0;
    }
    
    // Second pass: Build call graph
    if (!build_call_graph(analyzer)) {
        return 0;
    }
    
    // Third pass: Analyze functions in topological order
    if (!analyze_functions_topologically(analyzer)) {
        return 0;
    }
    
    // Fourth pass: Propagate escape information interprocedurally
    if (!propagate_escape_information(analyzer)) {
        return 0;
    }
    
    // Fifth pass: Optimize allocation strategies
    if (!optimize_allocation_strategies(analyzer)) {
        return 0;
    }
    
    return 1;
}

int collect_function_definitions(EscapeAnalyzer* analyzer, ASTNode* node) {
    if (!analyzer || !node) return 1;
    
    if (node->type == AST_FUNC_DECL) {
        FuncDeclNode* func_decl = (FuncDeclNode*)node;
        if (func_decl->name) {
            escape_analyzer_register_function(analyzer, func_decl->name, node);
        }
    }
    
    // Recursively process child nodes
    // TODO: Implement proper AST traversal for all node types
    
    return 1;
}

int build_call_graph(EscapeAnalyzer* analyzer) {
    if (!analyzer) return 0;
    
    // For each function, analyze its calls and build the call graph
    for (size_t i = 0; i < analyzer->function_count; i++) {
        FunctionEscapeInfo* func = analyzer->functions[i];
        if (func->function_node && func->function_node->type == AST_FUNC_DECL) {
            FuncDeclNode* func_decl = (FuncDeclNode*)func->function_node;
            if (func_decl->body) {
                collect_function_calls(analyzer, func_decl->body, func);
            }
        }
    }
    
    return 1;
}

int collect_function_calls(EscapeAnalyzer* analyzer, ASTNode* node, FunctionEscapeInfo* caller) {
    if (!analyzer || !node || !caller) return 1;
    
    if (node->type == AST_CALL_EXPR) {
        CallExprNode* call = (CallExprNode*)node;
        if (call->function && call->function->type == AST_IDENTIFIER) {
            IdentifierNode* callee_name = (IdentifierNode*)call->function;
            FunctionEscapeInfo* callee = escape_analyzer_find_function(analyzer, callee_name->name);
            
            if (callee) {
                // Add callee to caller's callee list
                // TODO: Implement dynamic array management for callees/callers
                caller->has_side_effects = 1; // Assume function calls have side effects
            }
        }
        
        escape_analyzer_register_call_site(analyzer, node);
    }
    
    // TODO: Recursively process child nodes for all AST node types
    
    return 1;
}

int analyze_functions_topologically(EscapeAnalyzer* analyzer) {
    if (!analyzer) return 0;
    
    // Simple approach: analyze all functions (in a real implementation,
    // we'd do topological sorting of the call graph)
    for (size_t i = 0; i < analyzer->function_count; i++) {
        analyze_function_escape(analyzer, analyzer->functions[i]);
    }
    
    return 1;
}

int propagate_escape_information(EscapeAnalyzer* analyzer) {
    if (!analyzer) return 0;
    
    // Iteratively propagate escape information until convergence
    int changed = 1;
    int iterations = 0;
    const int max_iterations = 10;
    
    while (changed && iterations < max_iterations) {
        changed = 0;
        iterations++;
        
        for (size_t i = 0; i < analyzer->function_count; i++) {
            // For each function call, propagate escape information
            for (size_t j = 0; j < analyzer->call_site_count; j++) {
                ASTNode* call_site = analyzer->call_sites[j];
                if (call_site->type == AST_CALL_EXPR) {
                    // TODO: Implement interprocedural escape propagation
                    // This would update parameter escape information based on
                    // how arguments are used in the caller
                }
            }
        }
    }
    
    return 1;
}

int optimize_allocation_strategies(EscapeAnalyzer* analyzer) {
    if (!analyzer) return 0;
    
    // Apply optimization heuristics
    for (size_t i = 0; i < analyzer->context_count; i++) {
        EscapeContext* context = analyzer->escape_contexts[i];
        
        // Optimize based on escape analysis results
        if (context->escape_kind == ESCAPE_NONE && 
            analyzer->aggressive_stack_allocation) {
            context->strategy = ALLOC_STRATEGY_STACK;
            context->lifetime = LIFETIME_LOCAL;
        }
        
        // Region-based allocation for related objects
        if (analyzer->enable_region_analysis &&
            context->escape_kind == ESCAPE_FUNCTION) {
            context->strategy = ALLOC_STRATEGY_REGION;
            analyzer->region_allocations_created++;
        }
        
        // Probabilistic optimization
        if (analyzer->enable_probabilistic_analysis &&
            context->escape_probability < 0.5) {
            // If escape probability is low, prefer stack allocation
            if (context->strategy == ALLOC_STRATEGY_HEAP) {
                context->strategy = ALLOC_STRATEGY_STACK;
                analyzer->stack_allocations_recommended++;
                analyzer->heap_allocations_required--;
            }
        }
    }
    
    return 1;
}

// =============================================================================
// Allocation Strategy Determination
// =============================================================================

AllocationStrategy determine_allocation_strategy(EscapeAnalyzer* analyzer, 
                                               ASTNode* alloc_site,
                                               EscapeKind escape_kind) {
    if (!analyzer || !alloc_site) return ALLOC_STRATEGY_HEAP;
    
    switch (escape_kind) {
        case ESCAPE_NONE:
            return ALLOC_STRATEGY_STACK;
            
        case ESCAPE_FUNCTION:
            if (analyzer->enable_region_analysis) {
                return ALLOC_STRATEGY_REGION;
            }
            return ALLOC_STRATEGY_HEAP;
            
        case ESCAPE_CLOSURE:
            return ALLOC_STRATEGY_HEAP;
            
        case ESCAPE_GLOBAL:
            return ALLOC_STRATEGY_GLOBAL;
            
        case ESCAPE_THREAD:
            return ALLOC_STRATEGY_THREAD_LOCAL;
            
        case ESCAPE_UNKNOWN:
        default:
            return ALLOC_STRATEGY_HEAP; // Conservative choice
    }
}

ObjectLifetime determine_object_lifetime(EscapeAnalyzer* analyzer,
                                        ASTNode* object_site,
                                        EscapeKind escape_kind) {
    if (!analyzer || !object_site) return LIFETIME_UNKNOWN;
    
    switch (escape_kind) {
        case ESCAPE_NONE:
            return LIFETIME_LOCAL;
            
        case ESCAPE_FUNCTION:
            return LIFETIME_RETURN;
            
        case ESCAPE_CLOSURE:
            return LIFETIME_CLOSURE_CAPTURE;
            
        case ESCAPE_GLOBAL:
            return LIFETIME_GLOBAL;
            
        case ESCAPE_THREAD:
            return LIFETIME_GLOBAL;
            
        case ESCAPE_UNKNOWN:
        default:
            return LIFETIME_UNKNOWN;
    }
}

// =============================================================================
// Statistics and Reporting
// =============================================================================

void escape_analyzer_print_statistics(EscapeAnalyzer* analyzer) {
    if (!analyzer) return;
    
    printf("=== Interprocedural Escape Analysis Statistics ===\n");
    printf("Functions analyzed: %zu\n", analyzer->functions_analyzed);
    printf("Escape contexts detected: %zu\n", analyzer->escapes_detected);
    printf("Stack allocations recommended: %zu\n", analyzer->stack_allocations_recommended);
    printf("Heap allocations required: %zu\n", analyzer->heap_allocations_required);
    printf("Region allocations created: %zu\n", analyzer->region_allocations_created);
    printf("Call sites analyzed: %zu\n", analyzer->call_site_count);
    printf("Errors: %d\n", analyzer->error_count);
    printf("Warnings: %d\n", analyzer->warning_count);
    
    if (analyzer->functions_analyzed > 0) {
        double stack_ratio = (double)analyzer->stack_allocations_recommended / 
                           (analyzer->stack_allocations_recommended + analyzer->heap_allocations_required);
        printf("Stack allocation ratio: %.2f%%\n", stack_ratio * 100.0);
    }
}

void escape_analyzer_print_function_info(EscapeAnalyzer* analyzer, const char* function_name) {
    if (!analyzer || !function_name) return;
    
    FunctionEscapeInfo* func = escape_analyzer_find_function(analyzer, function_name);
    if (!func) {
        printf("Function '%s' not found\n", function_name);
        return;
    }
    
    printf("=== Function Escape Analysis: %s ===\n", function_name);
    printf("Return escape: %s\n", escape_kind_to_string(func->return_escape));
    printf("Return lifetime: %s\n", object_lifetime_to_string(func->return_lifetime));
    printf("Is analyzed: %s\n", func->is_analyzed ? "yes" : "no");
    printf("Is recursive: %s\n", func->is_recursive ? "yes" : "no");
    printf("Has side effects: %s\n", func->has_side_effects ? "yes" : "no");
    printf("Complexity score: %zu\n", func->complexity_score);
    
    if (func->param_count > 0) {
        printf("Parameter escape information:\n");
        for (size_t i = 0; i < func->param_count; i++) {
            printf("  Param %zu: %s -> %s\n", i,
                   escape_kind_to_string(func->param_escape[i]),
                   allocation_strategy_to_string(func->param_alloc_strategy[i]));
        }
    }
}

// =============================================================================
// Utility Functions
// =============================================================================

const char* object_lifetime_to_string(ObjectLifetime lifetime) {
    switch (lifetime) {
        case LIFETIME_IMMEDIATE: return "immediate";
        case LIFETIME_LOCAL: return "local";
        case LIFETIME_PARAMETER: return "parameter";
        case LIFETIME_RETURN: return "return";
        case LIFETIME_CLOSURE_CAPTURE: return "closure_capture";
        case LIFETIME_GLOBAL: return "global";
        case LIFETIME_UNKNOWN: return "unknown";
        default: return "invalid";
    }
}

const char* allocation_strategy_to_string(AllocationStrategy strategy) {
    switch (strategy) {
        case ALLOC_STRATEGY_STACK: return "stack";
        case ALLOC_STRATEGY_HEAP: return "heap";
        case ALLOC_STRATEGY_REGION: return "region";
        case ALLOC_STRATEGY_THREAD_LOCAL: return "thread_local";
        case ALLOC_STRATEGY_GLOBAL: return "global";
        case ALLOC_STRATEGY_DEFER: return "defer";
        default: return "invalid";
    }
}

// =============================================================================
// Integration Functions
// =============================================================================

int integrate_escape_analysis_with_type_checker(TypeChecker* type_checker, EscapeAnalyzer* analyzer) {
    if (!type_checker || !analyzer) return 0;
    
    // Apply allocation strategies to type information
    for (size_t i = 0; i < analyzer->context_count; i++) {
        EscapeContext* context = analyzer->escape_contexts[i];
        
        // TODO: Update type checker with allocation strategy information
        // This would modify how the type checker handles memory allocation
        // for variables and expressions
        (void)context; // Suppress unused warning
    }
    
    return 1;
}

int apply_escape_analysis_to_codegen(EscapeAnalyzer* analyzer, ASTNode* program) {
    if (!analyzer || !program) return 0;
    
    // This function would be called during code generation to apply
    // the allocation strategies determined by escape analysis
    
    for (size_t i = 0; i < analyzer->context_count; i++) {
        EscapeContext* context = analyzer->escape_contexts[i];
        
        // Generate appropriate allocation code based on strategy
        switch (context->strategy) {
            case ALLOC_STRATEGY_STACK:
                // Generate stack allocation (alloca in LLVM)
                break;
                
            case ALLOC_STRATEGY_HEAP:
                // Generate heap allocation (malloc/new)
                break;
                
            case ALLOC_STRATEGY_REGION:
                // Generate region-based allocation
                break;
                
            case ALLOC_STRATEGY_THREAD_LOCAL:
                // Generate thread-local allocation
                break;
                
            case ALLOC_STRATEGY_GLOBAL:
                // Generate global/static allocation
                break;
                
            default:
                // Use default allocation strategy
                break;
        }
    }
    
    return 1;
}