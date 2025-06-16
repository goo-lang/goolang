#include "../include/auto_parallel.h"
#include "../include/ast.h"
#include "../include/types.h"
#include "../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

// Memory Access Pattern Analysis Implementation

// Extract variable accesses from a statement
void extract_variable_accesses(ASTNode* stmt, VariableAccess** reads, size_t* read_count,
                               VariableAccess** writes, size_t* write_count) {
    if (!stmt || !reads || !writes || !read_count || !write_count) {
        return;
    }
    
    // Initialize arrays
    size_t read_capacity = 10;
    size_t write_capacity = 10;
    *reads = (VariableAccess*)calloc(read_capacity, sizeof(VariableAccess));
    *writes = (VariableAccess*)calloc(write_capacity, sizeof(VariableAccess));
    *read_count = 0;
    *write_count = 0;
    
    if (!*reads || !*writes) {
        return;
    }
    
    // Extract accesses recursively
    extract_accesses_recursive(stmt, reads, read_count, &read_capacity,
                               writes, write_count, &write_capacity, false);
}

// Recursive helper to extract variable accesses from AST nodes
void extract_accesses_recursive(ASTNode* node, VariableAccess** reads, size_t* read_count, size_t* read_capacity,
                                VariableAccess** writes, size_t* write_count, size_t* write_capacity,
                                bool is_lvalue) {
    if (!node) {
        return;
    }
    
    switch (node->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)node;
            
            // Determine if this is a read or write access
            VariableAccess access = {0};
            if (ident->name) {
                access.variable_name = malloc(strlen(ident->name) + 1);
                if (access.variable_name) {
                    strcpy(access.variable_name, ident->name);
                }
            } else {
                access.variable_name = NULL;
            }
            access.access_type = is_lvalue ? ACCESS_WRITE : ACCESS_READ;
            access.array_index = NULL;
            access.is_indirect = false;
            access.is_constant_index = false;
            access.memory_address = NULL;
            
            // Add to appropriate array
            if (is_lvalue) {
                if (*write_count >= *write_capacity) {
                    *write_capacity *= 2;
                    *writes = (VariableAccess*)realloc(*writes, *write_capacity * sizeof(VariableAccess));
                }
                if (*writes) {
                    (*writes)[*write_count] = access;
                    (*write_count)++;
                }
            } else {
                if (*read_count >= *read_capacity) {
                    *read_capacity *= 2;
                    *reads = (VariableAccess*)realloc(*reads, *read_capacity * sizeof(VariableAccess));
                }
                if (*reads) {
                    (*reads)[*read_count] = access;
                    (*read_count)++;
                }
            }
            break;
        }
        
        case AST_INDEX_EXPR: {
            IndexExprNode* index_expr = (IndexExprNode*)node;
            
            // Array access - the array is being accessed
            if (index_expr->expr) {
                extract_accesses_recursive(index_expr->expr, reads, read_count, read_capacity,
                                          writes, write_count, write_capacity, is_lvalue);
            }
            
            // The index expression is always a read
            if (index_expr->index) {
                extract_accesses_recursive(index_expr->index, reads, read_count, read_capacity,
                                          writes, write_count, write_capacity, false);
            }
            break;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)node;
            
            // Handle assignment specially
            if (binary->operator == TOKEN_ASSIGN || binary->operator == TOKEN_SHORT_ASSIGN) {
                // Left side is a write
                if (binary->left) {
                    extract_accesses_recursive(binary->left, reads, read_count, read_capacity,
                                              writes, write_count, write_capacity, true);
                }
                // Right side is a read
                if (binary->right) {
                    extract_accesses_recursive(binary->right, reads, read_count, read_capacity,
                                              writes, write_count, write_capacity, false);
                }
            } else {
                // Both sides are reads for other operators
                if (binary->left) {
                    extract_accesses_recursive(binary->left, reads, read_count, read_capacity,
                                              writes, write_count, write_capacity, false);
                }
                if (binary->right) {
                    extract_accesses_recursive(binary->right, reads, read_count, read_capacity,
                                              writes, write_count, write_capacity, false);
                }
            }
            break;
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)node;
            if (unary->operand) {
                extract_accesses_recursive(unary->operand, reads, read_count, read_capacity,
                                          writes, write_count, write_capacity, is_lvalue);
            }
            break;
        }
        
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)node;
            // Function arguments are reads
            if (call->args) {
                extract_accesses_recursive(call->args, reads, read_count, read_capacity,
                                          writes, write_count, write_capacity, false);
            }
            break;
        }
        
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)node;
            if (block->statements) {
                extract_accesses_recursive(block->statements, reads, read_count, read_capacity,
                                          writes, write_count, write_capacity, is_lvalue);
            }
            break;
        }
        
        default:
            // For other node types, just traverse children if they exist
            break;
    }
    
    // Traverse to next sibling
    if (node->next) {
        extract_accesses_recursive(node->next, reads, read_count, read_capacity,
                                  writes, write_count, write_capacity, is_lvalue);
    }
}

// Check if two variable accesses may refer to the same memory location (alias analysis)
bool variables_may_alias(VariableAccess* access1, VariableAccess* access2) {
    if (!access1 || !access2) {
        return false;
    }
    
    // If both access the same variable name, they may alias
    if (access1->variable_name && access2->variable_name) {
        if (strcmp(access1->variable_name, access2->variable_name) == 0) {
            return true;
        }
    }
    
    // Conservative analysis: if either involves indirect access, assume they may alias
    if (access1->is_indirect || access2->is_indirect) {
        return true;
    }
    
    // For different variable names without indirection, assume no aliasing
    return false;
}

// Free variable access arrays
void free_variable_accesses(VariableAccess* accesses, size_t count) {
    if (!accesses) {
        return;
    }
    
    for (size_t i = 0; i < count; i++) {
        free(accesses[i].variable_name);
        free(accesses[i].array_index);
    }
    free(accesses);
}

// Check if a variable access depends on the loop induction variable
bool is_loop_variant_access(VariableAccess* access, LoopInfo* loop) {
    if (!access || !loop) {
        return false;
    }
    
    // Simple check: if array index contains the loop variable, it's loop-variant
    if (access->array_index) {
        // This would need more sophisticated analysis to parse index expressions
        // For now, assume any array access in a loop is potentially loop-variant
        return true;
    }
    
    return false;
}

// Dependency Graph Construction

// Build a dependency graph for a code block
DependencyGraph* build_dependency_graph(ParallelContext* ctx, ASTNode* code_block) {
    if (!ctx || !code_block) {
        return NULL;
    }
    
    DependencyGraph* graph = (DependencyGraph*)calloc(1, sizeof(DependencyGraph));
    if (!graph) {
        return NULL;
    }
    
    graph->node_count = 0;
    graph->capacity = 50;
    graph->nodes = (DependencyNode**)calloc(graph->capacity, sizeof(DependencyNode*));
    graph->has_cycles = false;
    
    if (!graph->nodes) {
        free(graph);
        return NULL;
    }
    
    // Analyze the code block for dependencies
    analyze_code_block_dependencies(graph, code_block);
    
    // Build adjacency matrix
    graph->adjacency_matrix = (int**)calloc(graph->node_count, sizeof(int*));
    if (graph->adjacency_matrix) {
        for (size_t i = 0; i < graph->node_count; i++) {
            graph->adjacency_matrix[i] = (int*)calloc(graph->node_count, sizeof(int));
        }
    }
    
    return graph;
}

// Analyze dependencies within a code block
void analyze_code_block_dependencies(DependencyGraph* graph, ASTNode* code_block) {
    if (!graph || !code_block) {
        return;
    }
    
    // Traverse statements and build dependency relationships
    ASTNode* current = code_block;
    size_t stmt_index = 0;
    
    while (current) {
        if (current->type == AST_EXPR_STMT || 
            current->type == AST_VAR_DECL ||
            current->type == AST_CALL_EXPR) {
            
            add_statement_to_graph(graph, current, stmt_index);
            stmt_index++;
        } else if (current->type == AST_BLOCK_STMT) {
            BlockStmtNode* block = (BlockStmtNode*)current;
            if (block->statements) {
                analyze_code_block_dependencies(graph, block->statements);
            }
        }
        
        current = current->next;
    }
    
    // Analyze dependencies between statements
    for (size_t i = 0; i < graph->node_count; i++) {
        for (size_t j = i + 1; j < graph->node_count; j++) {
            DependencyType dep_type = analyze_statement_dependency(
                graph->nodes[i]->statement, 
                graph->nodes[j]->statement
            );
            
            if (dep_type != DEP_NONE) {
                add_dependency_edge(graph, i, j, dep_type);
            }
        }
    }
}

// Add a statement to the dependency graph
void add_statement_to_graph(DependencyGraph* graph, ASTNode* stmt, size_t index) {
    if (!graph || !stmt) {
        return;
    }
    
    // Expand graph capacity if needed
    if (graph->node_count >= graph->capacity) {
        graph->capacity *= 2;
        graph->nodes = (DependencyNode**)realloc(graph->nodes, 
                                                graph->capacity * sizeof(DependencyNode*));
    }
    
    if (!graph->nodes) {
        return;
    }
    
    // Create dependency node
    DependencyNode* dep_node = (DependencyNode*)calloc(1, sizeof(DependencyNode));
    if (!dep_node) {
        return;
    }
    
    dep_node->statement = stmt;
    dep_node->statement_id = (int)index;
    dep_node->dep_type = DEP_NONE;
    dep_node->variable_name = NULL;
    dep_node->dependency_distance = 0;
    dep_node->source_access = NULL;
    dep_node->target_access = NULL;
    dep_node->next = NULL;
    
    graph->nodes[graph->node_count] = dep_node;
    graph->node_count++;
}

// Add a dependency edge between two statements
void add_dependency_edge(DependencyGraph* graph, size_t from_index, size_t to_index, DependencyType dep_type) {
    if (!graph || from_index >= graph->node_count || to_index >= graph->node_count) {
        return;
    }
    
    // Mark dependency in adjacency matrix if it exists
    if (graph->adjacency_matrix && graph->adjacency_matrix[from_index]) {
        graph->adjacency_matrix[from_index][to_index] = (int)dep_type;
    }
    
    // Update dependency node information
    if (graph->nodes[to_index]) {
        graph->nodes[to_index]->dep_type = dep_type;
    }
}

// Analyze dependency between two statements
DependencyType analyze_statement_dependency(ASTNode* stmt1, ASTNode* stmt2) {
    if (!stmt1 || !stmt2) {
        return DEP_NONE;
    }
    
    // Extract variable accesses from both statements
    VariableAccess* reads1 = NULL;
    VariableAccess* writes1 = NULL;
    size_t read_count1 = 0, write_count1 = 0;
    
    VariableAccess* reads2 = NULL;
    VariableAccess* writes2 = NULL;
    size_t read_count2 = 0, write_count2 = 0;
    
    extract_variable_accesses(stmt1, &reads1, &read_count1, &writes1, &write_count1);
    extract_variable_accesses(stmt2, &reads2, &read_count2, &writes2, &write_count2);
    
    DependencyType result = DEP_NONE;
    
    // Check for different types of dependencies
    
    // 1. True dependency (RAW): stmt1 writes, stmt2 reads the same location
    for (size_t i = 0; i < write_count1 && result == DEP_NONE; i++) {
        for (size_t j = 0; j < read_count2; j++) {
            if (variables_may_alias(&writes1[i], &reads2[j])) {
                result = DEP_TRUE;
                break;
            }
        }
    }
    
    // 2. Anti dependency (WAR): stmt1 reads, stmt2 writes the same location
    if (result == DEP_NONE) {
        for (size_t i = 0; i < read_count1 && result == DEP_NONE; i++) {
            for (size_t j = 0; j < write_count2; j++) {
                if (variables_may_alias(&reads1[i], &writes2[j])) {
                    result = DEP_ANTI;
                    break;
                }
            }
        }
    }
    
    // 3. Output dependency (WAW): both statements write to the same location
    if (result == DEP_NONE) {
        for (size_t i = 0; i < write_count1 && result == DEP_NONE; i++) {
            for (size_t j = 0; j < write_count2; j++) {
                if (variables_may_alias(&writes1[i], &writes2[j])) {
                    result = DEP_OUTPUT;
                    break;
                }
            }
        }
    }
    
    // 4. Input dependency (RAR): both statements read from the same location
    // This doesn't prevent parallelization, so we record it but don't block optimization
    if (result == DEP_NONE) {
        for (size_t i = 0; i < read_count1 && result == DEP_NONE; i++) {
            for (size_t j = 0; j < read_count2; j++) {
                if (variables_may_alias(&reads1[i], &reads2[j])) {
                    result = DEP_INPUT;
                    break;
                }
            }
        }
    }
    
    // Cleanup
    free_variable_accesses(reads1, read_count1);
    free_variable_accesses(writes1, write_count1);
    free_variable_accesses(reads2, read_count2);
    free_variable_accesses(writes2, write_count2);
    
    return result;
}

// Check if a loop has loop-carried dependencies
bool has_loop_carried_dependency(DependencyGraph* graph, LoopInfo* loop) {
    if (!graph || !loop) {
        return false;
    }
    
    // Analyze each dependency in the graph
    for (size_t i = 0; i < graph->node_count; i++) {
        DependencyNode* node = graph->nodes[i];
        if (!node) continue;
        
        // Check if this dependency crosses loop iterations
        if (can_prove_no_cross_iteration_dependence(node, loop)) {
            continue;
        }
        
        // If we can't prove independence, assume loop-carried dependency
        if (node->dep_type == DEP_TRUE || node->dep_type == DEP_ANTI || node->dep_type == DEP_OUTPUT) {
            return true;
        }
    }
    
    return false;
}

// Try to prove that a dependency doesn't cross loop iterations
bool can_prove_no_cross_iteration_dependence(DependencyNode* dep_node, LoopInfo* loop) {
    if (!dep_node || !loop) {
        return false;
    }
    
    // Simple analysis: if the dependency involves array accesses with the loop variable,
    // we need to check if different iterations access different elements
    
    // For now, implement a conservative approach
    // In a full implementation, this would involve sophisticated index analysis
    
    // If we can't analyze it, assume there might be a dependency
    return false;
}

// Free dependency graph
void dependency_graph_free(DependencyGraph* graph) {
    if (!graph) {
        return;
    }
    
    // Free dependency nodes
    if (graph->nodes) {
        for (size_t i = 0; i < graph->node_count; i++) {
            if (graph->nodes[i]) {
                free(graph->nodes[i]->variable_name);
                free(graph->nodes[i]);
            }
        }
        free(graph->nodes);
    }
    
    // Free adjacency matrix
    if (graph->adjacency_matrix) {
        for (size_t i = 0; i < graph->node_count; i++) {
            free(graph->adjacency_matrix[i]);
        }
        free(graph->adjacency_matrix);
    }
    
    free(graph);
}

// Utility function to print dependency graph (for debugging)
void print_dependency_graph(DependencyGraph* graph) {
    if (!graph) {
        printf("Dependency graph is NULL\n");
        return;
    }
    
    printf("\n=== Dependency Graph ===\n");
    printf("Nodes: %zu\n", graph->node_count);
    printf("Has cycles: %s\n", graph->has_cycles ? "Yes" : "No");
    
    for (size_t i = 0; i < graph->node_count; i++) {
        DependencyNode* node = graph->nodes[i];
        if (node) {
            printf("Node %zu: Statement %d, Type: %s\n", 
                   i, node->statement_id, dependency_type_string(node->dep_type));
        }
    }
    
    if (graph->adjacency_matrix) {
        printf("\nDependency Matrix:\n");
        for (size_t i = 0; i < graph->node_count; i++) {
            for (size_t j = 0; j < graph->node_count; j++) {
                printf("%d ", graph->adjacency_matrix[i][j]);
            }
            printf("\n");
        }
    }
    
    printf("========================\n\n");
}

// Get string representation of dependency type
const char* dependency_type_string(DependencyType type) {
    switch (type) {
        case DEP_NONE: return "None";
        case DEP_TRUE: return "True (RAW)";
        case DEP_ANTI: return "Anti (WAR)";
        case DEP_OUTPUT: return "Output (WAW)";
        case DEP_INPUT: return "Input (RAR)";
        case DEP_CONTROL: return "Control";
        case DEP_UNKNOWN: return "Unknown";
        default: return "Invalid";
    }
}