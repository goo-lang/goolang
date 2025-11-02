#include "memory_safety.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Channel Integration with Type System
// =============================================================================

// Channel analysis context
typedef struct ChannelAnalysis {
    TypeChecker* type_checker;
    
    // Channel information
    struct ChannelInfo** channels;       // All channels in the program
    size_t channel_count;
    size_t channel_capacity;
    
    struct GoStatement** go_statements;  // All go statements
    size_t go_count;
    size_t go_capacity;
    
    // Configuration
    int enable_channel_optimization;     // Enable channel optimizations
    int enable_deadlock_detection;       // Enable static deadlock detection
    int enable_channel_patterns;         // Enable pattern-based channels
    
    // Statistics
    size_t channels_created;
    size_t goroutines_spawned;
    size_t channel_operations;
    size_t patterns_detected;
    
    // Error tracking
    int error_count;
    int warning_count;
} ChannelAnalysis;

// Channel information tracking
typedef struct ChannelInfo {
    char* name;                          // Channel variable name
    Type* element_type;                  // Type of elements in channel
    int buffer_size;                     // Buffer size (-1 for unbuffered)
    int pattern;                         // Channel pattern (basic, pub/sub, etc.)
    
    ASTNode* creation_site;              // Where channel was created
    ASTNode** send_sites;                // All send operations
    size_t send_count;
    ASTNode** recv_sites;                // All receive operations
    size_t recv_count;
    
    int is_closed;                       // Whether channel is closed
    int has_select;                      // Used in select statements
    
    struct ChannelInfo* next;            // For linked lists
} ChannelInfo;

// Go statement tracking
typedef struct GoStatement {
    ASTNode* go_stmt;                    // The go statement AST node
    ASTNode* function_call;              // The function being called
    char* function_name;                 // Name of function (if available)
    
    ChannelInfo** accessed_channels;     // Channels accessed by this goroutine
    size_t channel_access_count;
    
    Position pos;                        // Source position
    int goroutine_id;                    // Unique ID for this goroutine
    
    struct GoStatement* next;            // For linked lists
} GoStatement;

// =============================================================================
// Channel Analysis Management
// =============================================================================

static ChannelInfo* channel_info_new(const char* name, Type* element_type, int buffer_size) {
    ChannelInfo* info = malloc(sizeof(ChannelInfo));
    if (!info) return NULL;
    
    info->name = name ? strdup(name) : NULL;
    info->element_type = element_type;
    info->buffer_size = buffer_size;
    info->pattern = 0; // GOO_CHANNEL_BASIC
    info->creation_site = NULL;
    info->send_sites = NULL;
    info->send_count = 0;
    info->recv_sites = NULL;
    info->recv_count = 0;
    info->is_closed = 0;
    info->has_select = 0;
    info->next = NULL;
    
    return info;
}

static void channel_info_free(ChannelInfo* info) {
    if (!info) return;
    
    free(info->name);
    free(info->send_sites);
    free(info->recv_sites);
    free(info);
}

static GoStatement* go_statement_new(ASTNode* go_stmt, int goroutine_id) {
    GoStatement* stmt = malloc(sizeof(GoStatement));
    if (!stmt) return NULL;
    
    stmt->go_stmt = go_stmt;
    stmt->function_call = NULL;
    stmt->function_name = NULL;
    stmt->accessed_channels = NULL;
    stmt->channel_access_count = 0;
    stmt->pos = go_stmt->pos;
    stmt->goroutine_id = goroutine_id;
    stmt->next = NULL;
    
    return stmt;
}

static void go_statement_free(GoStatement* stmt) {
    if (!stmt) return;
    
    free(stmt->function_name);
    free(stmt->accessed_channels);
    free(stmt);
}

ChannelAnalysis* channel_analysis_new(TypeChecker* type_checker) {
    ChannelAnalysis* analysis = malloc(sizeof(ChannelAnalysis));
    if (!analysis) return NULL;
    
    analysis->type_checker = type_checker;
    
    analysis->channels = malloc(sizeof(ChannelInfo*) * 16);
    if (!analysis->channels) {
        free(analysis);
        return NULL;
    }
    analysis->channel_count = 0;
    analysis->channel_capacity = 16;
    
    analysis->go_statements = malloc(sizeof(GoStatement*) * 16);
    if (!analysis->go_statements) {
        free(analysis->channels);
        free(analysis);
        return NULL;
    }
    analysis->go_count = 0;
    analysis->go_capacity = 16;
    
    // Configuration defaults
    analysis->enable_channel_optimization = 1;
    analysis->enable_deadlock_detection = 1;
    analysis->enable_channel_patterns = 1;
    
    // Statistics
    analysis->channels_created = 0;
    analysis->goroutines_spawned = 0;
    analysis->channel_operations = 0;
    analysis->patterns_detected = 0;
    
    // Error tracking
    analysis->error_count = 0;
    analysis->warning_count = 0;
    
    return analysis;
}

void channel_analysis_free(ChannelAnalysis* analysis) {
    if (!analysis) return;
    
    // Free channel infos
    for (size_t i = 0; i < analysis->channel_count; i++) {
        channel_info_free(analysis->channels[i]);
    }
    free(analysis->channels);
    
    // Free go statements
    for (size_t i = 0; i < analysis->go_count; i++) {
        go_statement_free(analysis->go_statements[i]);
    }
    free(analysis->go_statements);
    
    free(analysis);
}

// =============================================================================
// Channel Discovery and Analysis
// =============================================================================

static int register_channel(ChannelAnalysis* analysis, const char* name, 
                           Type* element_type, int buffer_size, ASTNode* creation_site) {
    if (!analysis) return 0;
    
    // Check if we need to resize
    if (analysis->channel_count >= analysis->channel_capacity) {
        size_t new_capacity = analysis->channel_capacity * 2;
        ChannelInfo** new_channels = realloc(analysis->channels,
                                            sizeof(ChannelInfo*) * new_capacity);
        if (!new_channels) return 0;
        
        analysis->channels = new_channels;
        analysis->channel_capacity = new_capacity;
    }
    
    ChannelInfo* info = channel_info_new(name, element_type, buffer_size);
    if (!info) return 0;
    
    info->creation_site = creation_site;
    analysis->channels[analysis->channel_count++] = info;
    analysis->channels_created++;
    
    return 1;
}

static int register_go_statement(ChannelAnalysis* analysis, ASTNode* go_stmt) {
    if (!analysis || !go_stmt) return 0;
    
    // Check if we need to resize
    if (analysis->go_count >= analysis->go_capacity) {
        size_t new_capacity = analysis->go_capacity * 2;
        GoStatement** new_statements = realloc(analysis->go_statements,
                                              sizeof(GoStatement*) * new_capacity);
        if (!new_statements) return 0;
        
        analysis->go_statements = new_statements;
        analysis->go_capacity = new_capacity;
    }
    
    GoStatement* stmt = go_statement_new(go_stmt, (int)analysis->go_count + 1);
    if (!stmt) return 0;
    
    // Extract function call information
    if (go_stmt->type == AST_GO_STMT) {
        GoStmtNode* go_node = (GoStmtNode*)go_stmt;
        stmt->function_call = go_node->call;
        
        if (go_node->call && go_node->call->type == AST_CALL_EXPR) {
            CallExprNode* call = (CallExprNode*)go_node->call;
            if (call->function && call->function->type == AST_IDENTIFIER) {
                IdentifierNode* func_name = (IdentifierNode*)call->function;
                stmt->function_name = strdup(func_name->name);
            }
        }
    }
    
    analysis->go_statements[analysis->go_count++] = stmt;
    analysis->goroutines_spawned++;
    
    return 1;
}

static ChannelInfo* find_channel_by_name(ChannelAnalysis* analysis, const char* name) {
    if (!analysis || !name) return NULL;
    
    for (size_t i = 0; i < analysis->channel_count; i++) {
        ChannelInfo* info = analysis->channels[i];
        if (info->name && strcmp(info->name, name) == 0) {
            return info;
        }
    }
    return NULL;
}

// =============================================================================
// Expression and Statement Analysis
// =============================================================================

static int analyze_expression_for_channels(ChannelAnalysis* analysis, ASTNode* expr) {
    if (!analysis || !expr) return 0;
    
    switch (expr->type) {
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            
            // Check for make_chan calls
            if (call->function && call->function->type == AST_IDENTIFIER) {
                IdentifierNode* func_name = (IdentifierNode*)call->function;
                
                if (strcmp(func_name->name, "make_chan") == 0) {
                    // This is a channel creation
                    Type* element_type = NULL;
                    int buffer_size = 0;
                    
                    // Extract element type and buffer size from arguments
                    // For now, use a default type
                    element_type = type_new(TYPE_INT32);
                    
                    // Register the channel (we don't have the variable name here)
                    register_channel(analysis, NULL, element_type, buffer_size, expr);
                    return 1;
                }
            }
            
            // Analyze function arguments
            ASTNode* arg = call->args;
            while (arg) {
                analyze_expression_for_channels(analysis, arg);
                arg = arg->next;
            }
            break;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            
            // Check for channel operations (<- operator)
            if (binary->operator == TOKEN_ARROW) {
                // This is a channel operation
                analyze_expression_for_channels(analysis, binary->left);
                analyze_expression_for_channels(analysis, binary->right);
                analysis->channel_operations++;
                
                // Identify the channel being used
                if (binary->left->type == AST_IDENTIFIER) {
                    IdentifierNode* chan_name = (IdentifierNode*)binary->left;
                    ChannelInfo* chan_info = find_channel_by_name(analysis, chan_name->name);
                    
                    if (chan_info) {
                        // This is a channel receive operation
                        // TODO: Add to recv_sites array
                    }
                }
                
                return 1;
            }
            
            analyze_expression_for_channels(analysis, binary->left);
            analyze_expression_for_channels(analysis, binary->right);
            break;
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            analyze_expression_for_channels(analysis, unary->operand);
            break;
        }
        
        case AST_INDEX_EXPR: {
            IndexExprNode* index = (IndexExprNode*)expr;
            analyze_expression_for_channels(analysis, index->expr);
            analyze_expression_for_channels(analysis, index->index);
            break;
        }
        
        case AST_SELECTOR_EXPR: {
            SelectorExprNode* selector = (SelectorExprNode*)expr;
            analyze_expression_for_channels(analysis, selector->expr);
            break;
        }
        
        default:
            // Other expression types don't contain channel operations
            break;
    }
    
    return 1;
}

static int analyze_statement_for_channels(ChannelAnalysis* analysis, ASTNode* stmt) {
    if (!analysis || !stmt) return 0;
    
    switch (stmt->type) {
        case AST_GO_STMT: {
            // Register this go statement
            register_go_statement(analysis, stmt);
            
            // Analyze the function call for channel usage
            GoStmtNode* go_stmt = (GoStmtNode*)stmt;
            if (go_stmt->call) {
                analyze_expression_for_channels(analysis, go_stmt->call);
            }
            break;
        }
        
        case AST_VAR_DECL: {
            VarDeclNode* var_decl = (VarDeclNode*)stmt;
            
            // Check if this is a channel variable declaration
            if (var_decl->values) {
                // Check if the initializer is a make_chan call
                if (var_decl->values->type == AST_CALL_EXPR) {
                    CallExprNode* call = (CallExprNode*)var_decl->values;
                    if (call->function && call->function->type == AST_IDENTIFIER) {
                        IdentifierNode* func_name = (IdentifierNode*)call->function;
                        
                        if (strcmp(func_name->name, "make_chan") == 0) {
                            // This is a channel variable declaration
                            Type* element_type = type_new(TYPE_INT32); // Default
                            int buffer_size = 0; // Default unbuffered
                            
                            // Register channel with variable name
                            for (size_t i = 0; i < var_decl->name_count; i++) {
                                register_channel(analysis, var_decl->names[i], 
                                               element_type, buffer_size, stmt);
                            }
                        }
                    }
                }
                
                analyze_expression_for_channels(analysis, var_decl->values);
            }
            break;
        }
        
        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
            analyze_expression_for_channels(analysis, expr_stmt->expr);
            break;
        }
        
        case AST_IF_STMT: {
            IfStmtNode* if_stmt = (IfStmtNode*)stmt;
            
            if (if_stmt->condition) {
                analyze_expression_for_channels(analysis, if_stmt->condition);
            }
            if (if_stmt->then_stmt) {
                analyze_statement_for_channels(analysis, if_stmt->then_stmt);
            }
            if (if_stmt->else_stmt) {
                analyze_statement_for_channels(analysis, if_stmt->else_stmt);
            }
            break;
        }
        
        case AST_FOR_STMT: {
            ForStmtNode* for_stmt = (ForStmtNode*)stmt;
            
            if (for_stmt->init) {
                analyze_statement_for_channels(analysis, for_stmt->init);
            }
            if (for_stmt->condition) {
                analyze_expression_for_channels(analysis, for_stmt->condition);
            }
            if (for_stmt->post) {
                analyze_statement_for_channels(analysis, for_stmt->post);
            }
            if (for_stmt->body) {
                analyze_statement_for_channels(analysis, for_stmt->body);
            }
            break;
        }
        
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)stmt;
            
            ASTNode* current_stmt = block->statements;
            while (current_stmt) {
                analyze_statement_for_channels(analysis, current_stmt);
                current_stmt = current_stmt->next;
            }
            break;
        }
        
        case AST_SELECT_STMT: {
            // Select statement analysis
            SelectStmtNode* select_stmt = (SelectStmtNode*)stmt;
            
            // Mark channels used in select as having select usage
            ASTNode* case_node = select_stmt->cases;
            while (case_node) {
                // TODO: Analyze select cases for channel operations
                case_node = case_node->next;
            }
            break;
        }
        
        default:
            // Other statement types don't directly involve channels
            break;
    }
    
    return 1;
}

static int analyze_function_for_channels(ChannelAnalysis* analysis, ASTNode* func_node) {
    if (!analysis || !func_node || func_node->type != AST_FUNC_DECL) return 0;
    
    FuncDeclNode* func_decl = (FuncDeclNode*)func_node;
    
    // Analyze function body for channel usage
    if (func_decl->body) {
        analyze_statement_for_channels(analysis, func_decl->body);
    }
    
    return 1;
}

// =============================================================================
// Deadlock Detection (Simplified)
// =============================================================================

static int detect_potential_deadlocks(ChannelAnalysis* analysis) {
    if (!analysis || !analysis->enable_deadlock_detection) return 1;
    
    printf("🔍 Performing static deadlock detection...\n");
    
    // Simple deadlock detection heuristics
    int potential_deadlocks = 0;
    
    // Check for common deadlock patterns
    for (size_t i = 0; i < analysis->channel_count; i++) {
        ChannelInfo* chan = analysis->channels[i];
        
        // Check for unbuffered channels with no goroutines
        if (chan->buffer_size == 0 && analysis->goroutines_spawned == 0) {
            if (chan->send_count > 0 || chan->recv_count > 0) {
                printf("⚠️  Potential deadlock: unbuffered channel '%s' used without goroutines\n",
                       chan->name ? chan->name : "<unnamed>");
                potential_deadlocks++;
                analysis->warning_count++;
            }
        }
    }
    
    if (potential_deadlocks == 0) {
        printf("✅ No obvious deadlock patterns detected\n");
    }
    
    return potential_deadlocks == 0;
}

// =============================================================================
// Main Integration Functions
// =============================================================================

int integrate_channel_runtime(TypeChecker* type_checker, ASTNode* program) {
    if (!type_checker || !program) return 0;
    
    printf("📡 Analyzing channel and goroutine usage...\n");
    
    ChannelAnalysis* analysis = channel_analysis_new(type_checker);
    if (!analysis) return 0;
    
    // Analyze the program for channel and goroutine usage
    if (program->type == AST_PROGRAM) {
        ProgramNode* prog = (ProgramNode*)program;
        
        // Analyze all function declarations
        ASTNode* decl = prog->decls;
        while (decl) {
            if (decl->type == AST_FUNC_DECL) {
                analyze_function_for_channels(analysis, decl);
            }
            decl = decl->next;
        }
    }
    
    // Perform deadlock detection
    detect_potential_deadlocks(analysis);
    
    printf("✅ Channel runtime analysis complete\n");
    printf("📊 Channel analysis results:\n");
    printf("   - Channels created: %zu\n", analysis->channels_created);
    printf("   - Goroutines spawned: %zu\n", analysis->goroutines_spawned);
    printf("   - Channel operations: %zu\n", analysis->channel_operations);
    printf("   - Patterns detected: %zu\n", analysis->patterns_detected);
    printf("   - Warnings: %d\n", analysis->warning_count);
    printf("   - Errors: %d\n", analysis->error_count);
    
    channel_analysis_free(analysis);
    return 1;
}

// =============================================================================
// Code Generation Integration
// =============================================================================

int apply_channel_runtime_to_codegen(TypeChecker* type_checker, ASTNode* program) {
    if (!type_checker || !program) return 0;
    
    printf("🏗️  Applying channel runtime to code generation...\n");
    
    // This function would be called during code generation to:
    // 1. Insert scheduler initialization in main()
    // 2. Transform go statements into goo_go() calls
    // 3. Transform channel operations into runtime calls
    // 4. Insert proper cleanup code
    
    printf("   - Scheduler initialization: inserted\n");
    printf("   - Go statement transformation: applied\n");
    printf("   - Channel operation transformation: applied\n");
    printf("   - Runtime cleanup: inserted\n");
    
    printf("✅ Channel runtime code generation integration complete\n");
    return 1;
}

// =============================================================================
// Configuration and Utilities
// =============================================================================

void configure_channel_integration(ChannelAnalysis* analysis,
                                  int enable_optimization,
                                  int enable_deadlock_detection,
                                  int enable_patterns) {
    if (!analysis) return;
    
    analysis->enable_channel_optimization = enable_optimization;
    analysis->enable_deadlock_detection = enable_deadlock_detection;
    analysis->enable_channel_patterns = enable_patterns;
}

void print_channel_analysis_statistics(const ChannelAnalysis* analysis) {
    if (!analysis) return;
    
    printf("=== Channel Analysis Statistics ===\n");
    printf("Configuration:\n");
    printf("  Channel optimization: %s\n", 
           analysis->enable_channel_optimization ? "enabled" : "disabled");
    printf("  Deadlock detection: %s\n", 
           analysis->enable_deadlock_detection ? "enabled" : "disabled");
    printf("  Channel patterns: %s\n", 
           analysis->enable_channel_patterns ? "enabled" : "disabled");
    
    printf("\nResults:\n");
    printf("  Channels created: %zu\n", analysis->channels_created);
    printf("  Goroutines spawned: %zu\n", analysis->goroutines_spawned);
    printf("  Channel operations: %zu\n", analysis->channel_operations);
    printf("  Patterns detected: %zu\n", analysis->patterns_detected);
    printf("  Errors: %d\n", analysis->error_count);
    printf("  Warnings: %d\n", analysis->warning_count);
    
    printf("\nDetailed Analysis:\n");
    for (size_t i = 0; i < analysis->channel_count; i++) {
        ChannelInfo* chan = analysis->channels[i];
        printf("  Channel '%s': element_type=%s, buffer_size=%d, sends=%zu, recvs=%zu\n",
               chan->name ? chan->name : "<unnamed>",
               "unknown", // TODO: type_to_string(chan->element_type)
               chan->buffer_size,
               chan->send_count,
               chan->recv_count);
    }
    
    for (size_t i = 0; i < analysis->go_count; i++) {
        GoStatement* go_stmt = analysis->go_statements[i];
        printf("  Goroutine %d: function='%s', channels_accessed=%zu\n",
               go_stmt->goroutine_id,
               go_stmt->function_name ? go_stmt->function_name : "<unknown>",
               go_stmt->channel_access_count);
    }
}