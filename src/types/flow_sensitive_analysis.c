#include "memory_safety.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Flow-sensitive ownership analyzer implementation

FlowSensitiveAnalyzer* flow_analyzer_new(TypeChecker* type_checker) {
    FlowSensitiveAnalyzer* analyzer = malloc(sizeof(FlowSensitiveAnalyzer));
    if (!analyzer) return NULL;
    
    analyzer->type_checker = type_checker;
    analyzer->cfg = NULL;
    analyzer->block_states = NULL;
    analyzer->worklist = NULL;
    analyzer->worklist_size = 0;
    
    // Default configuration
    analyzer->aggressive_optimization = 1;
    analyzer->safety_first = 1;
    analyzer->debug_mode = 0;
    
    // Initialize statistics
    analyzer->total_values_analyzed = 0;
    analyzer->moves_inferred = 0;
    analyzer->copies_required = 0;
    analyzer->borrows_inferred = 0;
    analyzer->unsafe_patterns_found = 0;
    analyzer->error_count = 0;
    analyzer->warning_count = 0;
    
    return analyzer;
}

void flow_analyzer_free(FlowSensitiveAnalyzer* analyzer) {
    if (!analyzer) return;
    
    if (analyzer->cfg) {
        cfg_free(analyzer->cfg);
    }
    
    if (analyzer->block_states) {
        // Free individual block states
        for (size_t i = 0; analyzer->cfg && i < analyzer->cfg->block_count; i++) {
            ownership_state_free(analyzer->block_states[i]);
        }
        free(analyzer->block_states);
    }
    
    free(analyzer->worklist);
    free(analyzer);
}

// Control Flow Graph Implementation

ControlFlowGraph* cfg_build(ASTNode* function_body) {
    if (!function_body) return NULL;
    
    ControlFlowGraph* cfg = malloc(sizeof(ControlFlowGraph));
    if (!cfg) return NULL;
    
    cfg->blocks = NULL;
    cfg->block_count = 0;
    cfg->block_capacity = 16;  // Initial capacity
    cfg->blocks = malloc(sizeof(BasicBlock*) * cfg->block_capacity);
    if (!cfg->blocks) {
        free(cfg);
        return NULL;
    }
    
    cfg->entry_block = cfg_add_block(cfg);
    cfg->exit_block = cfg_add_block(cfg);
    cfg->dominators = NULL;
    cfg->dominated_sets = NULL;
    cfg->loops = NULL;
    cfg->loop_count = 0;
    
    // Build CFG from AST - simplified implementation
    cfg_build_from_ast(cfg, function_body, cfg->entry_block, cfg->exit_block);
    
    // Compute dominator information
    cfg_compute_dominators(cfg);
    
    return cfg;
}

void cfg_free(ControlFlowGraph* cfg) {
    if (!cfg) return;
    
    // Free basic blocks
    for (size_t i = 0; i < cfg->block_count; i++) {
        BasicBlock* block = cfg->blocks[i];
        if (block) {
            free(block->statements);
            free(block->predecessors);
            free(block->successors);
            ownership_state_free(block->entry_state);
            ownership_state_free(block->exit_state);
            free(block);
        }
    }
    
    free(cfg->blocks);
    free(cfg->dominators);
    
    // Free dominated sets
    if (cfg->dominated_sets) {
        for (size_t i = 0; i < cfg->block_count; i++) {
            free(cfg->dominated_sets[i]);
        }
        free(cfg->dominated_sets);
    }
    
    // Free loop information
    if (cfg->loops) {
        for (size_t i = 0; i < cfg->loop_count; i++) {
            free(cfg->loops[i]);
        }
        free(cfg->loops);
    }
    
    free(cfg);
}

BasicBlock* cfg_add_block(ControlFlowGraph* cfg) {
    if (!cfg) return NULL;
    
    // Expand capacity if needed
    if (cfg->block_count >= cfg->block_capacity) {
        size_t new_cap = cfg->block_capacity * 2;
        BasicBlock** tmp = realloc(cfg->blocks, sizeof(BasicBlock*) * new_cap);
        if (!tmp) return NULL;
        cfg->blocks = tmp;
        cfg->block_capacity = new_cap;
    }
    
    BasicBlock* block = malloc(sizeof(BasicBlock));
    if (!block) return NULL;
    
    block->id = cfg->block_count;
    block->statements = NULL;
    block->statement_count = 0;
    block->predecessors = NULL;
    block->predecessor_count = 0;
    block->successors = NULL;
    block->successor_count = 0;
    block->entry_state = NULL;
    block->exit_state = NULL;
    block->visited = 0;
    block->dominates_exit = 0;
    
    cfg->blocks[cfg->block_count++] = block;
    return block;
}

void cfg_add_edge(BasicBlock* from, BasicBlock* to) {
    if (!from || !to) return;
    
    // Add to successors of 'from'
    BasicBlock** tmp_succ = realloc(from->successors,
                              sizeof(BasicBlock*) * (from->successor_count + 1));
    if (!tmp_succ) return;
    from->successors = tmp_succ;
    from->successors[from->successor_count++] = to;

    // Add to predecessors of 'to'
    BasicBlock** tmp_pred = realloc(to->predecessors,
                              sizeof(BasicBlock*) * (to->predecessor_count + 1));
    if (!tmp_pred) return;
    to->predecessors = tmp_pred;
    to->predecessors[to->predecessor_count++] = from;
}

// Simplified CFG building from AST
void cfg_build_from_ast(ControlFlowGraph* cfg, ASTNode* node, 
                       BasicBlock* current_block, BasicBlock* exit_block) {
    if (!cfg || !node || !current_block) return;
    
    switch (node->type) {
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)node;
            ASTNode* stmt = block->statements;
            
            while (stmt) {
                // Add statement to current block
                ASTNode** tmp_stmts = realloc(current_block->statements,
                    sizeof(ASTNode*) * (current_block->statement_count + 1));
                if (!tmp_stmts) break;
                current_block->statements = tmp_stmts;
                current_block->statements[current_block->statement_count++] = stmt;
                
                // Handle control flow changing statements
                if (stmt->type == AST_IF_STMT) {
                    IfStmtNode* if_stmt = (IfStmtNode*)stmt;
                    
                    BasicBlock* then_block = cfg_add_block(cfg);
                    BasicBlock* else_block = cfg_add_block(cfg);
                    BasicBlock* merge_block = cfg_add_block(cfg);
                    
                    cfg_add_edge(current_block, then_block);
                    cfg_add_edge(current_block, else_block);
                    
                    // Build then branch
                    cfg_build_from_ast(cfg, if_stmt->then_stmt, then_block, merge_block);
                    cfg_add_edge(then_block, merge_block);
                    
                    // Build else branch if it exists
                    if (if_stmt->else_stmt) {
                        cfg_build_from_ast(cfg, if_stmt->else_stmt, else_block, merge_block);
                    }
                    cfg_add_edge(else_block, merge_block);
                    
                    current_block = merge_block;
                }
                
                stmt = stmt->next;
            }
            
            // Connect to exit block if no explicit control flow
            if (current_block->successor_count == 0) {
                cfg_add_edge(current_block, exit_block);
            }
            break;
        }
        
        default: {
            // For other node types, just add to current block
            ASTNode** tmp_stmts = realloc(current_block->statements,
                sizeof(ASTNode*) * (current_block->statement_count + 1));
            if (!tmp_stmts) break;
            current_block->statements = tmp_stmts;
            current_block->statements[current_block->statement_count++] = node;
            break;
        }
    }
}

// Simplified dominator computation
void cfg_compute_dominators(ControlFlowGraph* cfg) {
    if (!cfg || cfg->block_count == 0) return;
    
    cfg->dominators = malloc(sizeof(BasicBlock*) * cfg->block_count);
    if (!cfg->dominators) return;
    
    // Initialize: entry block dominates itself, others are unknown
    for (size_t i = 0; i < cfg->block_count; i++) {
        cfg->dominators[i] = (i == 0) ? cfg->blocks[0] : NULL;
    }
    
    // Simplified iterative algorithm
    int changed = 1;
    while (changed) {
        changed = 0;
        
        for (size_t i = 1; i < cfg->block_count; i++) {  // Skip entry block
            BasicBlock* block = cfg->blocks[i];
            BasicBlock* new_dom = NULL;
            
            // Find common dominator of all predecessors
            for (size_t j = 0; j < block->predecessor_count; j++) {
                BasicBlock* pred = block->predecessors[j];
                size_t pred_id = pred->id;
                
                if (cfg->dominators[pred_id]) {
                    if (!new_dom) {
                        new_dom = cfg->dominators[pred_id];
                    } else {
                        // Find intersection (simplified)
                        new_dom = cfg->dominators[pred_id];
                    }
                }
            }
            
            if (cfg->dominators[i] != new_dom) {
                cfg->dominators[i] = new_dom;
                changed = 1;
            }
        }
    }
}

// Value State Management

ValueState* value_state_new(const char* name, ValueStateKind initial_state) {
    if (!name) return NULL;
    
    ValueState* state = malloc(sizeof(ValueState));
    if (!state) return NULL;
    
    state->name = malloc(strlen(name) + 1);
    if (!state->name) {
        free(state);
        return NULL;
    }
    strcpy(state->name, name);
    
    state->state = initial_state;
    state->recommended_transfer = TRANSFER_KIND_AUTO_INFERRED;
    state->escape_status = ESCAPE_NONE;
    state->last_use_position = 0;
    state->definition_position = 0;
    state->ref_count = 0;
    state->is_closure_capture = 0;
    state->path_states = NULL;
    state->path_count = 0;
    state->next = NULL;
    
    return state;
}

void value_state_free(ValueState* state) {
    if (!state) return;
    
    free(state->name);
    
    if (state->path_states) {
        for (size_t i = 0; i < state->path_count; i++) {
            value_state_free(state->path_states[i]);
        }
        free(state->path_states);
    }
    
    free(state);
}

ValueState* value_state_copy(const ValueState* state) {
    if (!state) return NULL;
    
    ValueState* copy = value_state_new(state->name, state->state);
    if (!copy) return NULL;
    
    copy->recommended_transfer = state->recommended_transfer;
    copy->escape_status = state->escape_status;
    copy->last_use_position = state->last_use_position;
    copy->definition_position = state->definition_position;
    copy->ref_count = state->ref_count;
    copy->is_closure_capture = state->is_closure_capture;
    
    // Copy path states if present
    if (state->path_states && state->path_count > 0) {
        copy->path_states = malloc(sizeof(ValueState*) * state->path_count);
        copy->path_count = state->path_count;
        
        for (size_t i = 0; i < state->path_count; i++) {
            copy->path_states[i] = value_state_copy(state->path_states[i]);
        }
    }
    
    return copy;
}

void value_state_merge(ValueState* target, const ValueState* source) {
    if (!target || !source) return;
    
    // Merge states conservatively
    if (target->state != source->state) {
        // If states differ, choose the more restrictive one
        if (source->state == VALUE_STATE_MOVED || target->state == VALUE_STATE_MOVED) {
            target->state = VALUE_STATE_CONDITIONALLY_MOVED;
        } else if (source->state == VALUE_STATE_UNINITIALIZED || 
                  target->state == VALUE_STATE_UNINITIALIZED) {
            target->state = VALUE_STATE_UNINITIALIZED;
        }
    }
    
    // Update reference count
    target->ref_count = (target->ref_count > source->ref_count) ? 
                       target->ref_count : source->ref_count;
    
    // More conservative escape analysis
    if (source->escape_status > target->escape_status) {
        target->escape_status = source->escape_status;
    }
}

// Ownership State Management

OwnershipState* ownership_state_new(OwnershipState* parent) {
    OwnershipState* state = malloc(sizeof(OwnershipState));
    if (!state) return NULL;
    
    state->values = NULL;
    state->value_count = 0;
    state->value_capacity = 8;  // Initial capacity
    state->values = malloc(sizeof(ValueState*) * state->value_capacity);
    if (!state->values) {
        free(state);
        return NULL;
    }
    
    state->copy_operations = 0;
    state->move_operations = 0;
    state->borrow_operations = 0;
    state->parent = parent;
    
    return state;
}

void ownership_state_free(OwnershipState* state) {
    if (!state) return;
    
    // Free all value states
    for (size_t i = 0; i < state->value_count; i++) {
        value_state_free(state->values[i]);
    }
    
    free(state->values);
    free(state);
}

OwnershipState* ownership_state_copy(const OwnershipState* state) {
    if (!state) return NULL;
    
    OwnershipState* copy = ownership_state_new(state->parent);
    if (!copy) return NULL;
    
    // Copy all value states
    for (size_t i = 0; i < state->value_count; i++) {
        ValueState* value_copy = value_state_copy(state->values[i]);
        if (value_copy) {
            ownership_state_add_value(copy, value_copy);
        }
    }
    
    copy->copy_operations = state->copy_operations;
    copy->move_operations = state->move_operations;
    copy->borrow_operations = state->borrow_operations;
    
    return copy;
}

void ownership_state_merge(OwnershipState* target, const OwnershipState* source) {
    if (!target || !source) return;
    
    // Merge value states
    for (size_t i = 0; i < source->value_count; i++) {
        ValueState* source_value = source->values[i];
        ValueState* target_value = ownership_state_lookup(target, source_value->name);
        
        if (target_value) {
            value_state_merge(target_value, source_value);
        } else {
            // Add new value state
            ValueState* new_value = value_state_copy(source_value);
            if (new_value) {
                ownership_state_add_value(target, new_value);
            }
        }
    }
    
    // Merge operation counts (pessimistically)
    target->copy_operations += source->copy_operations;
    target->move_operations += source->move_operations;
    target->borrow_operations += source->borrow_operations;
}

ValueState* ownership_state_lookup(OwnershipState* state, const char* name) {
    if (!state || !name) return NULL;
    
    // Search in current state
    for (size_t i = 0; i < state->value_count; i++) {
        if (strcmp(state->values[i]->name, name) == 0) {
            return state->values[i];
        }
    }
    
    // Search in parent state if not found
    if (state->parent) {
        return ownership_state_lookup(state->parent, name);
    }
    
    return NULL;
}

void ownership_state_update(OwnershipState* state, const char* name, ValueStateKind new_state) {
    if (!state || !name) return;
    
    ValueState* value = ownership_state_lookup(state, name);
    if (value) {
        value->state = new_state;
    } else {
        // Create new value state
        ValueState* new_value = value_state_new(name, new_state);
        if (new_value) {
            ownership_state_add_value(state, new_value);
        }
    }
}

void ownership_state_add_value(OwnershipState* state, ValueState* value) {
    if (!state || !value) return;
    
    // Expand capacity if needed
    if (state->value_count >= state->value_capacity) {
        size_t new_cap = state->value_capacity * 2;
        ValueState** tmp = realloc(state->values, sizeof(ValueState*) * new_cap);
        if (!tmp) return;
        state->values = tmp;
        state->value_capacity = new_cap;
    }
    
    state->values[state->value_count++] = value;
}

// Utility Functions

const char* value_state_to_string(ValueStateKind state) {
    switch (state) {
        case VALUE_STATE_UNINITIALIZED: return "uninitialized";
        case VALUE_STATE_INITIALIZED: return "initialized";
        case VALUE_STATE_MOVED: return "moved";
        case VALUE_STATE_BORROWED_IMMUTABLE: return "borrowed(immutable)";
        case VALUE_STATE_BORROWED_MUTABLE: return "borrowed(mutable)";
        case VALUE_STATE_PARTIAL_MOVE: return "partially_moved";
        case VALUE_STATE_CONDITIONALLY_MOVED: return "conditionally_moved";
        default: return "unknown";
    }
}

const char* transfer_kind_to_string(TransferKind kind) {
    switch (kind) {
        case TRANSFER_KIND_COPY: return "copy";
        case TRANSFER_KIND_MOVE: return "move";
        case TRANSFER_KIND_BORROW_IMMUTABLE: return "borrow";
        case TRANSFER_KIND_BORROW_MUTABLE: return "borrow_mut";
        case TRANSFER_KIND_AUTO_INFERRED: return "auto";
        default: return "unknown";
    }
}

const char* escape_kind_to_string(EscapeKind kind) {
    switch (kind) {
        case ESCAPE_NONE: return "none";
        case ESCAPE_FUNCTION: return "function";
        case ESCAPE_CLOSURE: return "closure";
        case ESCAPE_GLOBAL: return "global";
        case ESCAPE_THREAD: return "thread";
        case ESCAPE_UNKNOWN: return "unknown";
        default: return "unknown";
    }
}

// Error reporting
void flow_error(FlowSensitiveAnalyzer* analyzer, Position pos, const char* format, ...) {
    if (!analyzer) return;
    
    va_list args;
    va_start(args, format);
    
    fprintf(stderr, "Flow analysis error at %s:%d:%d: ", 
            pos.filename ? pos.filename : "<unknown>", pos.line, pos.column);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    va_end(args);
    analyzer->error_count++;
}

void flow_warning(FlowSensitiveAnalyzer* analyzer, Position pos, const char* format, ...) {
    if (!analyzer) return;
    
    va_list args;
    va_start(args, format);
    
    fprintf(stderr, "Flow analysis warning at %s:%d:%d: ", 
            pos.filename ? pos.filename : "<unknown>", pos.line, pos.column);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    va_end(args);
    analyzer->warning_count++;
}

// Debug and statistics
void flow_analyzer_print_statistics(FlowSensitiveAnalyzer* analyzer) {
    if (!analyzer) return;
    
    printf("=== Flow-Sensitive Analysis Statistics ===\n");
    printf("Total values analyzed: %zu\n", analyzer->total_values_analyzed);
    printf("Moves inferred: %zu\n", analyzer->moves_inferred);
    printf("Copies required: %zu\n", analyzer->copies_required);
    printf("Borrows inferred: %zu\n", analyzer->borrows_inferred);
    printf("Unsafe patterns found: %zu\n", analyzer->unsafe_patterns_found);
    printf("Errors: %d, Warnings: %d\n", analyzer->error_count, analyzer->warning_count);
    printf("==========================================\n");
}
