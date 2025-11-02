#include "error_context.h"
#include "types.h"
#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Compiler Integration for Automatic Error Context
// =============================================================================

// Context information for error union expressions
typedef struct ErrorUnionContext {
    ASTNode* try_expression;
    ASTNode* function_context;
    const char* operation_name;
    ErrorContextType context_type;
    bool requires_propagation;
} ErrorUnionContext;

// Global compiler integration state
static bool g_compiler_error_context_enabled = true;
static int g_current_function_depth = 0;
static ASTNode* g_current_function = NULL;

// =============================================================================
// Error Context Analysis during Type Checking
// =============================================================================

// Analyze a try expression and determine its error context
ErrorContextType analyze_try_expression_context(ASTNode* try_expr) {
    if (!try_expr || try_expr->type != AST_TRY_EXPRESSION) {
        return ERROR_CONTEXT_CUSTOM;
    }
    
    TryExpressionNode* try_node = (TryExpressionNode*)try_expr;
    if (!try_node->expression) {
        return ERROR_CONTEXT_CUSTOM;
    }
    
    // Analyze the inner expression to determine context type
    switch (try_node->expression->type) {
        case AST_FUNCTION_CALL: {
            FunctionCallNode* call = (FunctionCallNode*)try_node->expression;
            if (call->function_name) {
                const char* func_name = call->function_name;
                
                // File I/O functions
                if (strstr(func_name, "read") || strstr(func_name, "write") || 
                    strstr(func_name, "open") || strstr(func_name, "close") ||
                    strstr(func_name, "file")) {
                    return ERROR_CONTEXT_FILE_IO;
                }
                
                // Network functions
                if (strstr(func_name, "connect") || strstr(func_name, "send") ||
                    strstr(func_name, "recv") || strstr(func_name, "socket") ||
                    strstr(func_name, "http") || strstr(func_name, "tcp")) {
                    return ERROR_CONTEXT_NETWORK;
                }
                
                // Parsing functions
                if (strstr(func_name, "parse") || strstr(func_name, "decode") ||
                    strstr(func_name, "unmarshal") || strstr(func_name, "json") ||
                    strstr(func_name, "xml") || strstr(func_name, "toml")) {
                    return ERROR_CONTEXT_PARSING;
                }
                
                // Validation functions
                if (strstr(func_name, "validate") || strstr(func_name, "check") ||
                    strstr(func_name, "verify") || strstr(func_name, "assert")) {
                    return ERROR_CONTEXT_VALIDATION;
                }
                
                // Memory functions
                if (strstr(func_name, "alloc") || strstr(func_name, "malloc") ||
                    strstr(func_name, "free") || strstr(func_name, "realloc")) {
                    return ERROR_CONTEXT_MEMORY;
                }
            }
            break;
        }
        
        case AST_MEMBER_ACCESS: {
            MemberAccessNode* member = (MemberAccessNode*)try_node->expression;
            if (member->member_name) {
                // Context based on member access patterns
                if (strstr(member->member_name, "read") || strstr(member->member_name, "write")) {
                    return ERROR_CONTEXT_FILE_IO;
                }
                if (strstr(member->member_name, "send") || strstr(member->member_name, "recv")) {
                    return ERROR_CONTEXT_NETWORK;
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    return ERROR_CONTEXT_CUSTOM;
}

// Generate error context code for a try expression
char* generate_error_context_code(ASTNode* try_expr, ErrorContextType context_type) {
    if (!try_expr) return NULL;
    
    const char* context_name = "unknown";
    const char* operation_desc = "operation";
    
    switch (context_type) {
        case ERROR_CONTEXT_FILE_IO:
            context_name = "FILE_IO";
            operation_desc = "file operation";
            break;
        case ERROR_CONTEXT_NETWORK:
            context_name = "NETWORK";
            operation_desc = "network operation";
            break;
        case ERROR_CONTEXT_PARSING:
            context_name = "PARSING";
            operation_desc = "parsing operation";
            break;
        case ERROR_CONTEXT_VALIDATION:
            context_name = "VALIDATION";
            operation_desc = "validation operation";
            break;
        case ERROR_CONTEXT_MEMORY:
            context_name = "MEMORY";
            operation_desc = "memory operation";
            break;
        case ERROR_CONTEXT_COMPUTATION:
            context_name = "COMPUTATION";
            operation_desc = "computation";
            break;
        case ERROR_CONTEXT_CONCURRENCY:
            context_name = "CONCURRENCY";
            operation_desc = "concurrency operation";
            break;
        default:
            context_name = "CUSTOM";
            operation_desc = "operation";
            break;
    }
    
    // Generate the context frame setup code
    size_t code_size = 512;
    char* code = malloc(code_size);
    if (!code) return NULL;
    
    snprintf(code, code_size,
        "ErrorStackFrame* __error_frame_%p = error_context_push_frame(\n"
        "    __func__, __FILE__, __LINE__, ERROR_CONTEXT_%s, \"%s\");\n"
        "if (!__error_frame_%p) { /* Handle frame creation failure */ }\n",
        (void*)try_expr, context_name, operation_desc, (void*)try_expr);
    
    return code;
}

// Generate error propagation code
char* generate_error_propagation_code(ASTNode* try_expr) {
    if (!try_expr) return NULL;
    
    size_t code_size = 256;
    char* code = malloc(code_size);
    if (!code) return NULL;
    
    snprintf(code, code_size,
        "if (goo_error_union_is_error(__result_%p)) {\n"
        "    error_context_pop_frame();\n"
        "    return propagate_error_with_context(__result_%p);\n"
        "}\n"
        "error_context_pop_frame();\n",
        (void*)try_expr, (void*)try_expr);
    
    return code;
}

// =============================================================================
// Type Checker Integration
// =============================================================================

// Check if a function should have automatic error context
bool function_needs_error_context(FunctionNode* func_node) {
    if (!func_node) return false;
    
    // Functions with error union return types need context
    if (func_node->return_type && 
        func_node->return_type->node_type == TYPE_ERROR_UNION) {
        return true;
    }
    
    // Functions with try expressions in their body need context
    return function_contains_try_expressions(func_node->body);
}

// Recursively check if a statement block contains try expressions
bool function_contains_try_expressions(ASTNode* stmt_node) {
    if (!stmt_node) return false;
    
    switch (stmt_node->type) {
        case AST_TRY_EXPRESSION:
            return true;
            
        case AST_COMPOUND_STATEMENT: {
            CompoundStatementNode* compound = (CompoundStatementNode*)stmt_node;
            for (int i = 0; i < compound->statement_count; i++) {
                if (function_contains_try_expressions(compound->statements[i])) {
                    return true;
                }
            }
            break;
        }
        
        case AST_IF_STATEMENT: {
            IfStatementNode* if_stmt = (IfStatementNode*)stmt_node;
            if (function_contains_try_expressions(if_stmt->condition) ||
                function_contains_try_expressions(if_stmt->then_statement) ||
                function_contains_try_expressions(if_stmt->else_statement)) {
                return true;
            }
            break;
        }
        
        case AST_WHILE_STATEMENT: {
            WhileStatementNode* while_stmt = (WhileStatementNode*)stmt_node;
            if (function_contains_try_expressions(while_stmt->condition) ||
                function_contains_try_expressions(while_stmt->body)) {
                return true;
            }
            break;
        }
        
        case AST_FOR_STATEMENT: {
            ForStatementNode* for_stmt = (ForStatementNode*)stmt_node;
            if (function_contains_try_expressions(for_stmt->initialization) ||
                function_contains_try_expressions(for_stmt->condition) ||
                function_contains_try_expressions(for_stmt->update) ||
                function_contains_try_expressions(for_stmt->body)) {
                return true;
            }
            break;
        }
        
        case AST_RETURN_STATEMENT: {
            ReturnStatementNode* ret_stmt = (ReturnStatementNode*)stmt_node;
            if (function_contains_try_expressions(ret_stmt->expression)) {
                return true;
            }
            break;
        }
        
        case AST_EXPRESSION_STATEMENT: {
            ExpressionStatementNode* expr_stmt = (ExpressionStatementNode*)stmt_node;
            if (function_contains_try_expressions(expr_stmt->expression)) {
                return true;
            }
            break;
        }
        
        default:
            // Check child nodes for other node types
            break;
    }
    
    return false;
}

// Transform try expressions to include automatic error context
ASTNode* transform_try_expression_with_context(ASTNode* try_expr, TypeChecker* checker) {
    if (!try_expr || try_expr->type != AST_TRY_EXPRESSION) {
        return try_expr;
    }
    
    if (!g_compiler_error_context_enabled) {
        return try_expr;
    }
    
    TryExpressionNode* try_node = (TryExpressionNode*)try_expr;
    
    // Analyze the context type
    ErrorContextType context_type = analyze_try_expression_context(try_expr);
    
    // Generate context setup code
    char* setup_code = generate_error_context_code(try_expr, context_type);
    char* propagation_code = generate_error_propagation_code(try_expr);
    
    if (!setup_code || !propagation_code) {
        free(setup_code);
        free(propagation_code);
        return try_expr;
    }
    
    // Create a new compound statement that includes:
    // 1. Error context setup
    // 2. Original try expression
    // 3. Error propagation logic
    
    CompoundStatementNode* enhanced_stmt = ast_compound_statement_new();
    if (!enhanced_stmt) {
        free(setup_code);
        free(propagation_code);
        return try_expr;
    }
    
    // Add setup code as raw C code injection (for now)
    // TODO: Create proper AST nodes for error context operations
    ExpressionStatementNode* setup_stmt = ast_expression_statement_new(NULL);
    // Store the setup code in a comment or custom node for code generation
    
    // Add the original try expression
    ExpressionStatementNode* try_stmt = ast_expression_statement_new((ASTNode*)try_node);
    
    // Add propagation code
    ExpressionStatementNode* prop_stmt = ast_expression_statement_new(NULL);
    
    ast_compound_statement_add_statement(enhanced_stmt, (ASTNode*)setup_stmt);
    ast_compound_statement_add_statement(enhanced_stmt, (ASTNode*)try_stmt);
    ast_compound_statement_add_statement(enhanced_stmt, (ASTNode*)prop_stmt);
    
    free(setup_code);
    free(propagation_code);
    
    return (ASTNode*)enhanced_stmt;
}

// =============================================================================
// Compiler Pass for Error Context Enhancement
// =============================================================================

// Compiler pass to enhance functions with automatic error context
void enhance_functions_with_error_context(ASTNode* root, TypeChecker* checker) {
    if (!root || !g_compiler_error_context_enabled) {
        return;
    }
    
    switch (root->type) {
        case AST_FUNCTION_DECLARATION: {
            FunctionNode* func_node = (FunctionNode*)root;
            
            if (function_needs_error_context(func_node)) {
                g_current_function = root;
                g_current_function_depth++;
                
                // Transform the function body
                if (func_node->body) {
                    func_node->body = enhance_statement_with_error_context(
                        func_node->body, checker);
                }
                
                g_current_function_depth--;
                if (g_current_function_depth == 0) {
                    g_current_function = NULL;
                }
            }
            break;
        }
        
        case AST_PROGRAM: {
            ProgramNode* program = (ProgramNode*)root;
            for (int i = 0; i < program->declaration_count; i++) {
                enhance_functions_with_error_context(program->declarations[i], checker);
            }
            break;
        }
        
        default:
            break;
    }
}

// Recursively enhance statements with error context
ASTNode* enhance_statement_with_error_context(ASTNode* stmt, TypeChecker* checker) {
    if (!stmt) return NULL;
    
    switch (stmt->type) {
        case AST_TRY_EXPRESSION:
            return transform_try_expression_with_context(stmt, checker);
            
        case AST_COMPOUND_STATEMENT: {
            CompoundStatementNode* compound = (CompoundStatementNode*)stmt;
            for (int i = 0; i < compound->statement_count; i++) {
                compound->statements[i] = enhance_statement_with_error_context(
                    compound->statements[i], checker);
            }
            break;
        }
        
        case AST_IF_STATEMENT: {
            IfStatementNode* if_stmt = (IfStatementNode*)stmt;
            if_stmt->then_statement = enhance_statement_with_error_context(
                if_stmt->then_statement, checker);
            if (if_stmt->else_statement) {
                if_stmt->else_statement = enhance_statement_with_error_context(
                    if_stmt->else_statement, checker);
            }
            break;
        }
        
        case AST_WHILE_STATEMENT: {
            WhileStatementNode* while_stmt = (WhileStatementNode*)stmt;
            while_stmt->body = enhance_statement_with_error_context(
                while_stmt->body, checker);
            break;
        }
        
        case AST_FOR_STATEMENT: {
            ForStatementNode* for_stmt = (ForStatementNode*)stmt;
            for_stmt->body = enhance_statement_with_error_context(
                for_stmt->body, checker);
            break;
        }
        
        default:
            break;
    }
    
    return stmt;
}

// =============================================================================
// Code Generation Integration
// =============================================================================

// Generate C code for error context operations
void codegen_error_context_setup(FILE* output, ASTNode* try_expr, ErrorContextType context_type) {
    if (!output || !try_expr) return;
    
    const char* context_name = error_context_type_to_string(context_type);
    
    fprintf(output, 
        "    // Automatic error context setup\n"
        "    ErrorStackFrame* __error_frame_%p = error_context_push_frame(\n"
        "        __func__, __FILE__, __LINE__, ERROR_CONTEXT_%s, \"%s operation\");\n",
        (void*)try_expr, context_name, context_name);
}

void codegen_error_context_cleanup(FILE* output, ASTNode* try_expr) {
    if (!output || !try_expr) return;
    
    fprintf(output,
        "    // Automatic error context cleanup\n"
        "    error_context_pop_frame();\n");
}

void codegen_error_propagation_with_context(FILE* output, ASTNode* try_expr) {
    if (!output || !try_expr) return;
    
    fprintf(output,
        "    // Automatic error propagation with context\n"
        "    if (goo_error_union_is_error(__result_%p)) {\n"
        "        error_context_pop_frame();\n"
        "        return propagate_error_with_context(__result_%p);\n"
        "    }\n",
        (void*)try_expr, (void*)try_expr);
}

// =============================================================================
// Utility Functions
// =============================================================================

const char* error_context_type_to_string(ErrorContextType type) {
    switch (type) {
        case ERROR_CONTEXT_FILE_IO: return "FILE_IO";
        case ERROR_CONTEXT_NETWORK: return "NETWORK";
        case ERROR_CONTEXT_PARSING: return "PARSING";
        case ERROR_CONTEXT_VALIDATION: return "VALIDATION";
        case ERROR_CONTEXT_COMPUTATION: return "COMPUTATION";
        case ERROR_CONTEXT_MEMORY: return "MEMORY";
        case ERROR_CONTEXT_CONCURRENCY: return "CONCURRENCY";
        case ERROR_CONTEXT_CUSTOM: return "CUSTOM";
        default: return "UNKNOWN";
    }
}

void enable_compiler_error_context(bool enable) {
    g_compiler_error_context_enabled = enable;
}

bool is_compiler_error_context_enabled(void) {
    return g_compiler_error_context_enabled;
}

// =============================================================================
// Integration with Existing Type Checker
// =============================================================================

void integrate_error_context_with_type_checker(TypeChecker* checker) {
    if (!checker) return;
    
    // Add error context enhancement as a post-processing step
    // This would be called after the main type checking pass
    
    printf("🔍 Integrating automatic error context with type checker\n");
    
    // Register error context transformation pass
    // TODO: Add this to the compiler pipeline
}

void register_compiler_error_contexts(void) {
    error_context_system_init();
    
    printf("🔍 Registered compiler error contexts\n");
    
    // Register common error contexts used by the compiler
    // This could be expanded to include custom user-defined contexts
}