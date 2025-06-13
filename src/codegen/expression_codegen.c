#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Expression code generation

ValueInfo* codegen_generate_expression(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    if (!codegen || !checker || !expr) return NULL;
    
    switch (expr->type) {
        case AST_IDENTIFIER:
            return codegen_generate_identifier(codegen, checker, expr);
        case AST_LITERAL:
            return codegen_generate_literal(codegen, checker, expr);
        case AST_BINARY_EXPR:
            return codegen_generate_binary_expr(codegen, checker, expr);
        case AST_UNARY_EXPR:
            return codegen_generate_unary_expr(codegen, checker, expr);
        case AST_CALL_EXPR:
            return codegen_generate_call_expr(codegen, checker, expr);
        case AST_INDEX_EXPR:
            return codegen_generate_index_expr(codegen, checker, expr);
        case AST_SELECTOR_EXPR:
            return codegen_generate_selector_expr(codegen, checker, expr);
        case AST_TRY_EXPR:
            return codegen_generate_try_expr(codegen, checker, expr);
        case AST_CATCH_EXPR:
            return codegen_generate_catch_expr(codegen, checker, expr);
        case AST_PTR_ARITHMETIC:
            return codegen_generate_ptr_arithmetic(codegen, checker, expr);
        case AST_PTR_DEREF:
            return codegen_generate_ptr_deref(codegen, checker, expr);
        case AST_ADDR_OF:
            return codegen_generate_addr_of(codegen, checker, expr);
        case AST_PORT_IO:
            return codegen_generate_port_io(codegen, checker, expr);
        case AST_MMIO_ACCESS:
            return codegen_generate_mmio_access(codegen, checker, expr);
        default:
            codegen_error(codegen, expr->pos, "Unknown expression type for code generation");
            return NULL;
    }
}

ValueInfo* codegen_generate_identifier(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_IDENTIFIER) return NULL;
    
    IdentifierNode* ident = (IdentifierNode*)expr;
    
    // Look up the identifier in the symbol table
    ValueInfo* value_info = codegen_lookup_value(codegen, ident->name);
    if (!value_info) {
        codegen_error(codegen, expr->pos, "Undefined identifier '%s'", ident->name);
        return NULL;
    }
    
    // If it's an lvalue (variable), load the value
    if (value_info->is_lvalue) {
        LLVMValueRef loaded_value = LLVMBuildLoad2(codegen->builder, LLVMTypeOf(value_info->llvm_value), value_info->llvm_value, ident->name);
        return value_info_new(ident->name, loaded_value, value_info->goo_type);
    } else {
        // Direct value (constant, function, etc.)
        return value_info_new(ident->name, value_info->llvm_value, value_info->goo_type);
    }
#endif
}

ValueInfo* codegen_generate_literal(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_LITERAL) return NULL;
    
    LiteralNode* literal = (LiteralNode*)expr;
    LLVMValueRef llvm_value = NULL;
    Type* goo_type = NULL;
    
    switch (literal->literal_type) {
        case TOKEN_INT: {
            // Parse integer value from string
            long long value = atoll(literal->value);
            llvm_value = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), value, 1);
            goo_type = type_checker_get_builtin(checker, TYPE_INT64);
            break;
        }
        
        case TOKEN_FLOAT: {
            // Parse float value from string
            double value = atof(literal->value);
            llvm_value = LLVMConstReal(LLVMDoubleTypeInContext(codegen->context), value);
            goo_type = type_checker_get_builtin(checker, TYPE_FLOAT64);
            break;
        }
        
        case TOKEN_STRING: {
            // Create global string constant
            LLVMValueRef str_const = LLVMBuildGlobalStringPtr(codegen->builder, literal->value, "str");
            
            // Create string struct { ptr, len }
            LLVMTypeRef string_type = codegen_get_basic_type(codegen, TYPE_STRING);
            LLVMValueRef string_val = LLVMGetUndef(string_type);
            
            // Set pointer
            string_val = LLVMBuildInsertValue(codegen->builder, string_val, str_const, 0, "");
            
            // Set length
            size_t len = strlen(literal->value);
            LLVMValueRef len_val = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), len, 0);
            string_val = LLVMBuildInsertValue(codegen->builder, string_val, len_val, 1, "");
            
            llvm_value = string_val;
            goo_type = type_checker_get_builtin(checker, TYPE_STRING);
            break;
        }
        
        case TOKEN_TRUE:
        case TOKEN_FALSE: {
            int bool_val = (literal->literal_type == TOKEN_TRUE) ? 1 : 0;
            llvm_value = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), bool_val, 0);
            goo_type = type_checker_get_builtin(checker, TYPE_BOOL);
            break;
        }
        
        case TOKEN_CHAR: {
            // Parse character value (for now, just take first character)
            char char_val = literal->value[0];
            llvm_value = LLVMConstInt(LLVMInt8TypeInContext(codegen->context), char_val, 0);
            goo_type = type_checker_get_builtin(checker, TYPE_CHAR);
            break;
        }
        
        default:
            codegen_error(codegen, expr->pos, "Unknown literal type");
            return NULL;
    }
    
    if (!llvm_value || !goo_type) {
        codegen_error(codegen, expr->pos, "Failed to generate literal");
        return NULL;
    }
    
    return value_info_new(NULL, llvm_value, goo_type);
#endif
}

ValueInfo* codegen_generate_binary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_BINARY_EXPR) return NULL;
    
    BinaryExprNode* binary = (BinaryExprNode*)expr;
    
    // Special handling for assignment
    if (binary->operator == TOKEN_ASSIGN) {
        // Left side must be an lvalue
        if (binary->left->type != AST_IDENTIFIER) {
            codegen_error(codegen, expr->pos, "Assignment target must be an identifier");
            return NULL;
        }
        
        IdentifierNode* ident = (IdentifierNode*)binary->left;
        ValueInfo* target = codegen_lookup_value(codegen, ident->name);
        if (!target || !target->is_lvalue) {
            codegen_error(codegen, expr->pos, "Invalid assignment target '%s'", ident->name);
            return NULL;
        }
        
        // Generate right side value
        ValueInfo* value = codegen_generate_expression(codegen, checker, binary->right);
        if (!value) return NULL;
        
        // Store the value
        LLVMBuildStore(codegen->builder, value->llvm_value, target->llvm_value);
        
        // Return the stored value
        return value;
    }
    
    // Special handling for channel send (<-)
    if (binary->operator == TOKEN_ARROW) {
        return codegen_generate_channel_send(codegen, checker, expr);
    }
    
    // Generate left and right operands
    ValueInfo* left_val = codegen_generate_expression(codegen, checker, binary->left);
    if (!left_val) return NULL;
    
    ValueInfo* right_val = codegen_generate_expression(codegen, checker, binary->right);
    if (!right_val) {
        value_info_free(left_val);
        return NULL;
    }
    
    // Get the result type from the type checker
    Type* result_type = type_check_binary_expr(checker, expr);
    if (!result_type) {
        value_info_free(left_val);
        value_info_free(right_val);
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    LLVMValueRef left_llvm = left_val->llvm_value;
    LLVMValueRef right_llvm = right_val->llvm_value;
    
    // Generate operation based on operator and types
    switch (binary->operator) {
        // Arithmetic operators
        case TOKEN_PLUS:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildAdd(codegen->builder, left_llvm, right_llvm, "add");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFAdd(codegen->builder, left_llvm, right_llvm, "fadd");
            }
            break;
            
        case TOKEN_MINUS:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildSub(codegen->builder, left_llvm, right_llvm, "sub");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFSub(codegen->builder, left_llvm, right_llvm, "fsub");
            }
            break;
            
        case TOKEN_MULTIPLY:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildMul(codegen->builder, left_llvm, right_llvm, "mul");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFMul(codegen->builder, left_llvm, right_llvm, "fmul");
            }
            break;
            
        case TOKEN_DIVIDE:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildSDiv(codegen->builder, left_llvm, right_llvm, "sdiv");
                } else {
                    result = LLVMBuildUDiv(codegen->builder, left_llvm, right_llvm, "udiv");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFDiv(codegen->builder, left_llvm, right_llvm, "fdiv");
            }
            break;
            
        case TOKEN_MODULO:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildSRem(codegen->builder, left_llvm, right_llvm, "srem");
                } else {
                    result = LLVMBuildURem(codegen->builder, left_llvm, right_llvm, "urem");
                }
            }
            break;
            
        // Comparison operators
        case TOKEN_EQ:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildICmp(codegen->builder, LLVMIntEQ, left_llvm, right_llvm, "eq");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOEQ, left_llvm, right_llvm, "feq");
            }
            break;
            
        case TOKEN_NE:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildICmp(codegen->builder, LLVMIntNE, left_llvm, right_llvm, "ne");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealONE, left_llvm, right_llvm, "fne");
            }
            break;
            
        case TOKEN_LT:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSLT, left_llvm, right_llvm, "slt");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntULT, left_llvm, right_llvm, "ult");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOLT, left_llvm, right_llvm, "flt");
            }
            break;
            
        case TOKEN_LE:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSLE, left_llvm, right_llvm, "sle");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntULE, left_llvm, right_llvm, "ule");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOLE, left_llvm, right_llvm, "fle");
            }
            break;
            
        case TOKEN_GT:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSGT, left_llvm, right_llvm, "sgt");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntUGT, left_llvm, right_llvm, "ugt");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOGT, left_llvm, right_llvm, "fgt");
            }
            break;
            
        case TOKEN_GE:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSGE, left_llvm, right_llvm, "sge");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntUGE, left_llvm, right_llvm, "uge");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOGE, left_llvm, right_llvm, "fge");
            }
            break;
            
        // Logical operators
        case TOKEN_AND:
            result = LLVMBuildAnd(codegen->builder, left_llvm, right_llvm, "and");
            break;
            
        case TOKEN_OR:
            result = LLVMBuildOr(codegen->builder, left_llvm, right_llvm, "or");
            break;
            
        // Bitwise operators
        case TOKEN_BIT_AND:
            result = LLVMBuildAnd(codegen->builder, left_llvm, right_llvm, "bitand");
            break;
            
        case TOKEN_BIT_OR:
            result = LLVMBuildOr(codegen->builder, left_llvm, right_llvm, "bitor");
            break;
            
        case TOKEN_BIT_XOR:
            result = LLVMBuildXor(codegen->builder, left_llvm, right_llvm, "xor");
            break;
            
        case TOKEN_LSHIFT:
            result = LLVMBuildShl(codegen->builder, left_llvm, right_llvm, "shl");
            break;
            
        case TOKEN_RSHIFT:
            if (type_is_signed(left_val->goo_type)) {
                result = LLVMBuildAShr(codegen->builder, left_llvm, right_llvm, "ashr");
            } else {
                result = LLVMBuildLShr(codegen->builder, left_llvm, right_llvm, "lshr");
            }
            break;
            
        default:
            codegen_error(codegen, expr->pos, "Unsupported binary operator");
            value_info_free(left_val);
            value_info_free(right_val);
            return NULL;
    }
    
    value_info_free(left_val);
    value_info_free(right_val);
    
    if (!result) {
        codegen_error(codegen, expr->pos, "Failed to generate binary operation");
        return NULL;
    }
    
    return value_info_new(NULL, result, result_type);
#endif
}

ValueInfo* codegen_generate_unary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_UNARY_EXPR) return NULL;
    
    UnaryExprNode* unary = (UnaryExprNode*)expr;
    
    // Special handling for channel receive
    if (unary->operator == TOKEN_ARROW) {
        return codegen_generate_channel_recv(codegen, checker, expr);
    }
    
    // Generate operand
    ValueInfo* operand = codegen_generate_expression(codegen, checker, unary->operand);
    if (!operand) return NULL;
    
    // Get result type
    Type* result_type = type_check_unary_expr(checker, expr);
    if (!result_type) {
        value_info_free(operand);
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    LLVMValueRef operand_llvm = operand->llvm_value;
    
    switch (unary->operator) {
        case TOKEN_PLUS:
            // Unary plus is a no-op
            result = operand_llvm;
            break;
            
        case TOKEN_MINUS:
            if (type_is_integer(operand->goo_type)) {
                result = LLVMBuildNeg(codegen->builder, operand_llvm, "neg");
            } else if (type_is_float(operand->goo_type)) {
                result = LLVMBuildFNeg(codegen->builder, operand_llvm, "fneg");
            }
            break;
            
        case TOKEN_NOT:
            result = LLVMBuildNot(codegen->builder, operand_llvm, "not");
            break;
            
        case TOKEN_MULTIPLY:
            // Dereference pointer
            if (operand->goo_type->kind == TYPE_POINTER) {
                result = LLVMBuildLoad2(codegen->builder, LLVMGetElementType(LLVMTypeOf(operand_llvm)), operand_llvm, "deref");
            } else {
                codegen_error(codegen, expr->pos, "Cannot dereference non-pointer type");
                value_info_free(operand);
                return NULL;
            }
            break;
            
        case TOKEN_AND: {
            // Address-of operator - operand must be an lvalue
            if (!operand->is_lvalue) {
                codegen_error(codegen, expr->pos, "Cannot take address of non-lvalue");
                value_info_free(operand);
                return NULL;
            }
            
            // The operand is already a pointer to the value (lvalue)
            result = operand->llvm_value;
            
            // Create pointer type
            Type* ptr_type = type_new(TYPE_POINTER);
            ptr_type->data.pointer.pointee_type = operand->goo_type;
            ptr_type->size = 8;  // Assuming 64-bit pointers
            ptr_type->align = 8;
            result_type = ptr_type;
            break;
        }
            
        default:
            codegen_error(codegen, expr->pos, "Unsupported unary operator");
            value_info_free(operand);
            return NULL;
    }
    
    value_info_free(operand);
    
    if (!result) {
        codegen_error(codegen, expr->pos, "Failed to generate unary operation");
        return NULL;
    }
    
    return value_info_new(NULL, result, result_type);
#endif
}

ValueInfo* codegen_generate_call_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    
    CallExprNode* call = (CallExprNode*)expr;
    
    // Check for built-in functions first
    if (call->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_name = (IdentifierNode*)call->function;
        if (strcmp(func_name->name, "make_chan") == 0) {
            return codegen_generate_make_chan_call(codegen, checker, expr);
        }
    }
    
    // Generate function expression
    ValueInfo* func_val = codegen_generate_expression(codegen, checker, call->function);
    if (!func_val) return NULL;
    
    // Generate arguments
    size_t arg_count = 0;
    ASTNode* arg = call->args;
    while (arg) {
        arg_count++;
        arg = arg->next;
    }
    
    LLVMValueRef* args = NULL;
    if (arg_count > 0) {
        args = malloc(sizeof(LLVMValueRef) * arg_count);
        if (!args) {
            value_info_free(func_val);
            return NULL;
        }
        
        arg = call->args;
        for (size_t i = 0; i < arg_count; i++) {
            ValueInfo* arg_val = codegen_generate_expression(codegen, checker, arg);
            if (!arg_val) {
                for (size_t j = 0; j < i; j++) {
                    // Note: We should free the ValueInfo structures too, but we don't have them here
                }
                free(args);
                value_info_free(func_val);
                return NULL;
            }
            args[i] = arg_val->llvm_value;
            value_info_free(arg_val);
            arg = arg->next;
        }
    }
    
    // Generate call
    LLVMValueRef result = LLVMBuildCall2(codegen->builder, LLVMGetElementType(LLVMTypeOf(func_val->llvm_value)), func_val->llvm_value, args, (unsigned)arg_count, "call");
    
    free(args);
    
    // Get return type
    Type* return_type = type_check_call_expr(checker, expr);
    value_info_free(func_val);
    
    if (!return_type) {
        return NULL;
    }
    
    return value_info_new(NULL, result, return_type);
#endif
}

ValueInfo* codegen_generate_index_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_INDEX_EXPR) return NULL;
    
    IndexExprNode* index_expr = (IndexExprNode*)expr;
    
    // Generate code for the base expression (array/slice/map)
    ValueInfo* base_val = codegen_generate_expression(codegen, checker, index_expr->expr);
    if (!base_val) {
        codegen_error(codegen, expr->pos, "Failed to generate base expression for index");
        return NULL;
    }
    
    // Generate code for the index expression
    ValueInfo* index_val = codegen_generate_expression(codegen, checker, index_expr->index);
    if (!index_val) {
        codegen_error(codegen, expr->pos, "Failed to generate index expression");
        value_info_free(base_val);
        return NULL;
    }
    
    Type* base_type = base_val->goo_type;
    Type* element_type = NULL;
    LLVMValueRef result = NULL;
    
    // Handle different indexed types
    switch (base_type->kind) {
        case TYPE_ARRAY: {
            element_type = base_type->data.array.element_type;
            
            // For arrays, generate GEP
            LLVMValueRef indices[] = {
                LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),  // Array base
                index_val->llvm_value  // Array index
            };
            
            if (base_val->is_lvalue) {
                // Base is a pointer to array
                result = LLVMBuildGEP2(codegen->builder,
                                      codegen_type_to_llvm(codegen, base_type),
                                      base_val->llvm_value, indices, 2, "array_elem");
            } else {
                // Base is array value, need to create alloca
                LLVMValueRef array_alloca = codegen_create_alloca(codegen,
                                                                 codegen_type_to_llvm(codegen, base_type),
                                                                 "array_tmp");
                LLVMBuildStore(codegen->builder, base_val->llvm_value, array_alloca);
                result = LLVMBuildGEP2(codegen->builder,
                                      codegen_type_to_llvm(codegen, base_type),
                                      array_alloca, indices, 2, "array_elem");
            }
            break;
        }
        
        case TYPE_SLICE: {
            element_type = base_type->data.slice.element_type;
            
            // Slices are structs with { ptr, len }
            // Extract the data pointer
            LLVMValueRef slice_ptr;
            if (base_val->is_lvalue) {
                // Load the slice struct
                LLVMValueRef slice_val = LLVMBuildLoad2(codegen->builder,
                                                       codegen_type_to_llvm(codegen, base_type),
                                                       base_val->llvm_value, "slice_load");
                slice_ptr = LLVMBuildExtractValue(codegen->builder, slice_val, 0, "slice_ptr");
            } else {
                slice_ptr = LLVMBuildExtractValue(codegen->builder, base_val->llvm_value, 0, "slice_ptr");
            }
            
            // Generate bounds check if in safe mode
            // TODO: Add runtime bounds checking
            
            // Index into the slice data
            result = LLVMBuildGEP2(codegen->builder,
                                  codegen_type_to_llvm(codegen, element_type),
                                  slice_ptr, &index_val->llvm_value, 1, "slice_elem");
            break;
        }
        
        case TYPE_STRING: {
            // Strings are like slices with byte elements
            element_type = type_checker_get_builtin(checker, TYPE_UINT8);
            
            // Extract the data pointer from string struct
            LLVMValueRef string_ptr;
            if (base_val->is_lvalue) {
                LLVMValueRef string_val = LLVMBuildLoad2(codegen->builder,
                                                        codegen_type_to_llvm(codegen, base_type),
                                                        base_val->llvm_value, "string_load");
                string_ptr = LLVMBuildExtractValue(codegen->builder, string_val, 0, "string_ptr");
            } else {
                string_ptr = LLVMBuildExtractValue(codegen->builder, base_val->llvm_value, 0, "string_ptr");
            }
            
            // Index into the string data
            result = LLVMBuildGEP2(codegen->builder,
                                  LLVMInt8TypeInContext(codegen->context),
                                  string_ptr, &index_val->llvm_value, 1, "string_char");
            break;
        }
        
        case TYPE_MAP: {
            // TODO: Implement map indexing with runtime call
            codegen_error(codegen, expr->pos, "Map indexing not yet implemented");
            value_info_free(base_val);
            value_info_free(index_val);
            return NULL;
        }
        
        case TYPE_POINTER: {
            // Pointer indexing (pointer arithmetic)
            element_type = base_type->data.pointer.pointee_type;
            
            LLVMValueRef ptr_val = base_val->llvm_value;
            if (base_val->is_lvalue) {
                ptr_val = LLVMBuildLoad2(codegen->builder,
                                       codegen_type_to_llvm(codegen, base_type),
                                       base_val->llvm_value, "ptr_load");
            }
            
            result = LLVMBuildGEP2(codegen->builder,
                                  codegen_type_to_llvm(codegen, element_type),
                                  ptr_val, &index_val->llvm_value, 1, "ptr_elem");
            break;
        }
        
        default:
            codegen_error(codegen, expr->pos, "Type cannot be indexed");
            value_info_free(base_val);
            value_info_free(index_val);
            return NULL;
    }
    
    // Create value info for the result
    ValueInfo* result_val = value_info_new(NULL, result, element_type);
    result_val->is_lvalue = 1;  // Indexing returns an lvalue
    
    value_info_free(base_val);
    value_info_free(index_val);
    
    return result_val;
#endif
}

ValueInfo* codegen_generate_selector_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_SELECTOR_EXPR) return NULL;
    
    SelectorExprNode* selector = (SelectorExprNode*)expr;
    
    // Generate code for the base expression
    ValueInfo* base_val = codegen_generate_expression(codegen, checker, selector->expr);
    if (!base_val) {
        codegen_error(codegen, expr->pos, "Failed to generate base expression for selector");
        return NULL;
    }
    
    // Get the type of the base expression
    Type* base_type = base_val->goo_type;
    
    // Handle pointer to struct
    if (base_type->kind == TYPE_POINTER && base_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        base_type = base_type->data.pointer.pointee_type;
        // Load through the pointer if needed
        if (!base_val->is_lvalue) {
            // If it's not an lvalue, we need to load it first
            LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, 
                                                codegen_type_to_llvm(codegen, base_type),
                                                base_val->llvm_value, "ptr_load");
            base_val->llvm_value = loaded;
        }
    }
    
    if (base_type->kind != TYPE_STRUCT) {
        codegen_error(codegen, expr->pos, "Selector can only be applied to struct types");
        value_info_free(base_val);
        return NULL;
    }
    
    // Find the field in the struct
    int field_index = -1;
    StructField* field = NULL;
    for (size_t i = 0; i < base_type->data.struct_type.field_count; i++) {
        if (strcmp(base_type->data.struct_type.fields[i].name, selector->selector) == 0) {
            field_index = (int)i;
            field = &base_type->data.struct_type.fields[i];
            break;
        }
    }
    
    if (field_index == -1) {
        codegen_error(codegen, expr->pos, "Field '%s' not found in struct", selector->selector);
        value_info_free(base_val);
        return NULL;
    }
    
    // Generate GEP instruction to get pointer to field
    LLVMValueRef indices[] = {
        LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),  // Deref struct pointer
        LLVMConstInt(LLVMInt32TypeInContext(codegen->context), field_index, 0)  // Field index
    };
    
    LLVMValueRef field_ptr;
    if (base_val->is_lvalue) {
        // Base is already a pointer to the struct
        field_ptr = LLVMBuildGEP2(codegen->builder, 
                                 codegen_type_to_llvm(codegen, base_type),
                                 base_val->llvm_value, indices, 2, selector->selector);
    } else {
        // Base is a struct value, need to create alloca first
        LLVMValueRef struct_alloca = codegen_create_alloca(codegen, 
                                                          codegen_type_to_llvm(codegen, base_type), 
                                                          "struct_tmp");
        LLVMBuildStore(codegen->builder, base_val->llvm_value, struct_alloca);
        field_ptr = LLVMBuildGEP2(codegen->builder, 
                                 codegen_type_to_llvm(codegen, base_type),
                                 struct_alloca, indices, 2, selector->selector);
    }
    
    // Create value info for the field
    ValueInfo* field_val = value_info_new(selector->selector, field_ptr, field->type);
    field_val->is_lvalue = 1;  // Field access returns an lvalue
    
    value_info_free(base_val);
    return field_val;
#endif
}

// Goo extension expressions

// Forward declarations for error union functions
ValueInfo* codegen_generate_try_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_catch_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

ValueInfo* codegen_generate_try_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    return codegen_generate_try_expr_impl(codegen, checker, expr);
}

ValueInfo* codegen_generate_catch_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    return codegen_generate_catch_expr_impl(codegen, checker, expr);
}

ValueInfo* codegen_generate_channel_send(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if LLVM_AVAILABLE
    if (!codegen || !checker || !expr || expr->type != AST_BINARY_EXPR) return NULL;
    
    BinaryExprNode* binary = (BinaryExprNode*)expr;
    
    // Generate channel and value expressions
    ValueInfo* channel_val = codegen_generate_expression(codegen, checker, binary->left);
    if (!channel_val) return NULL;
    
    ValueInfo* value_val = codegen_generate_expression(codegen, checker, binary->right);
    if (!value_val) return NULL;
    
    // Get element size and create call to goo_chan_send
    // For now, assume int type - this should be determined from the channel type
    LLVMValueRef elem_size __attribute__((unused)) = LLVMConstInt(LLVMInt64Type(), sizeof(int), 0);
    
    // Cast value to void pointer
    LLVMValueRef value_ptr = value_val->llvm_value;
    if (!value_val->is_lvalue) {
        // Need to store the value temporarily
        LLVMValueRef temp_alloca = LLVMBuildAlloca(codegen->builder, 
                                                   LLVMTypeOf(value_val->llvm_value), 
                                                   "temp_send_value");
        LLVMBuildStore(codegen->builder, value_val->llvm_value, temp_alloca);
        value_ptr = temp_alloca;
    }
    
    // Cast to void*
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    value_ptr = LLVMBuildBitCast(codegen->builder, value_ptr, void_ptr_type, "value_as_void_ptr");
    
    // Get the goo_chan_send function
    LLVMTypeRef param_types[] = {
        void_ptr_type,  // goo_channel_t*
        void_ptr_type   // void* data
    };
    LLVMTypeRef send_func_type = LLVMFunctionType(LLVMInt32Type(), param_types, 2, 0);
    
    LLVMValueRef send_func = LLVMGetNamedFunction(codegen->module, "goo_chan_send");
    if (!send_func) {
        // Declare goo_chan_send if not already declared
        send_func = LLVMAddFunction(codegen->module, "goo_chan_send", send_func_type);
    }
    
    // Call goo_chan_send(channel, value_ptr)
    LLVMValueRef args[] = { channel_val->llvm_value, value_ptr };
    LLVMValueRef result = LLVMBuildCall2(codegen->builder, send_func_type, send_func, args, 2, "send_result");
    
    // Create result value info
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = type_checker_get_builtin(checker, TYPE_INT32);  // Returns int (success/failure)
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    return result_info;
#else
    codegen_error(codegen, expr->pos, "Channel send operations require LLVM");
    return NULL;
#endif
}

ValueInfo* codegen_generate_channel_recv(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if LLVM_AVAILABLE
    if (!codegen || !checker || !expr || expr->type != AST_UNARY_EXPR) return NULL;
    
    UnaryExprNode* unary = (UnaryExprNode*)expr;
    
    // Generate channel expression
    ValueInfo* channel_val = codegen_generate_expression(codegen, checker, unary->operand);
    if (!channel_val) return NULL;
    
    // For receive, we need to determine the element type from the channel type
    // For now, assume int type - this should be determined from the channel type
    LLVMTypeRef element_type = LLVMInt32Type();
    
    // Allocate space for the received value
    LLVMValueRef result_alloca = LLVMBuildAlloca(codegen->builder, element_type, "recv_result");
    
    // Cast result alloca to void*
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    LLVMValueRef result_ptr = LLVMBuildBitCast(codegen->builder, result_alloca, void_ptr_type, "result_as_void_ptr");
    
    // Get the goo_chan_recv function
    LLVMTypeRef param_types_recv[] = {
        void_ptr_type,  // goo_channel_t*
        void_ptr_type   // void* data
    };
    LLVMTypeRef recv_func_type = LLVMFunctionType(LLVMInt32Type(), param_types_recv, 2, 0);
    
    LLVMValueRef recv_func = LLVMGetNamedFunction(codegen->module, "goo_chan_recv");
    if (!recv_func) {
        // Declare goo_chan_recv if not already declared
        recv_func = LLVMAddFunction(codegen->module, "goo_chan_recv", recv_func_type);
    }
    
    // Call goo_chan_recv(channel, result_ptr)
    LLVMValueRef args[] = { channel_val->llvm_value, result_ptr };
    LLVMValueRef success __attribute__((unused)) = LLVMBuildCall2(codegen->builder, recv_func_type, recv_func, args, 2, "recv_success");
    
    // For simplicity, assume the receive always succeeds and load the value
    // In a complete implementation, we'd need to check the success flag
    LLVMValueRef received_value = LLVMBuildLoad2(codegen->builder, element_type, result_alloca, "received_value");
    
    // Create result value info
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = received_value;
    result_info->goo_type = type_checker_get_builtin(checker, TYPE_INT32);  // Should match channel element type
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    return result_info;
#else
    codegen_error(codegen, expr->pos, "Channel receive operations require LLVM");
    return NULL;
#endif
}

ValueInfo* codegen_generate_make_chan_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if LLVM_AVAILABLE
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    
    CallExprNode* call = (CallExprNode*)expr;
    
    // Count arguments: make_chan(type, buffer_size)
    size_t arg_count = 0;
    ASTNode* arg = call->args;
    while (arg) {
        arg_count++;
        arg = arg->next;
    }
    
    if (arg_count < 1 || arg_count > 2) {
        codegen_error(codegen, expr->pos, "make_chan requires 1 or 2 arguments: make_chan(type) or make_chan(type, buffer_size)");
        return NULL;
    }
    
    // For now, assume element size of int (4 bytes) - this should be determined from the type argument
    LLVMValueRef elem_size = LLVMConstInt(LLVMInt64Type(), sizeof(int), 0);
    
    // Get buffer size (default to 0 for unbuffered channel)
    LLVMValueRef buffer_size = LLVMConstInt(LLVMInt64Type(), 0, 0);
    if (arg_count == 2) {
        // Generate the buffer size argument
        ASTNode* size_arg = call->args->next;  // Second argument
        ValueInfo* size_val = codegen_generate_expression(codegen, checker, size_arg);
        if (!size_val) return NULL;
        
        // Convert to size_t if needed
        buffer_size = size_val->llvm_value;
        if (LLVMTypeOf(buffer_size) != LLVMInt64Type()) {
            buffer_size = LLVMBuildZExt(codegen->builder, buffer_size, LLVMInt64Type(), "buffer_size_ext");
        }
        value_info_free(size_val);
    }
    
    // Get the goo_make_chan function
    LLVMTypeRef param_types[] = {
        LLVMInt64Type(),  // size_t elem_size
        LLVMInt64Type()   // size_t buffer_size
    };
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    LLVMTypeRef make_chan_func_type = LLVMFunctionType(void_ptr_type, param_types, 2, 0);
    
    LLVMValueRef make_chan_func = LLVMGetNamedFunction(codegen->module, "goo_make_chan");
    if (!make_chan_func) {
        // Declare goo_make_chan if not already declared
        make_chan_func = LLVMAddFunction(codegen->module, "goo_make_chan", make_chan_func_type);
    }
    
    // Call goo_make_chan(elem_size, buffer_size)
    LLVMValueRef args[] = { elem_size, buffer_size };
    LLVMValueRef channel = LLVMBuildCall2(codegen->builder, make_chan_func_type, make_chan_func, args, 2, "new_channel");
    
    // Create result value info
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = channel;
    result_info->goo_type = NULL;  // Should be a channel type
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    return result_info;
#else
    codegen_error(codegen, expr->pos, "Channel creation requires LLVM");
    return NULL;
#endif
}

// Unsafe operation implementations

ValueInfo* codegen_generate_ptr_arithmetic(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_PTR_ARITHMETIC) return NULL;
    
    PtrArithmeticNode* ptr_arith = (PtrArithmeticNode*)expr;
    
    // Generate pointer expression
    ValueInfo* ptr_val = codegen_generate_expression(codegen, checker, ptr_arith->pointer);
    if (!ptr_val) {
        codegen_error(codegen, expr->pos, "Failed to generate pointer expression");
        return NULL;
    }
    
    // Generate offset expression
    ValueInfo* offset_val = codegen_generate_expression(codegen, checker, ptr_arith->offset);
    if (!offset_val) {
        value_info_free(ptr_val);
        codegen_error(codegen, expr->pos, "Failed to generate offset expression");
        return NULL;
    }
    
    // Perform pointer arithmetic using GEP
    LLVMValueRef result = LLVMBuildGEP2(codegen->builder,
                                        LLVMInt8Type(),  // i8 for byte arithmetic
                                        ptr_val->llvm_value,
                                        &offset_val->llvm_value, 1,
                                        "ptr_arith");
    
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = ptr_val->goo_type; // Same type as input pointer
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(ptr_val);
    value_info_free(offset_val);
    
    return result_info;
#endif
}

ValueInfo* codegen_generate_ptr_deref(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_PTR_DEREF) return NULL;
    
    PtrDerefNode* ptr_deref = (PtrDerefNode*)expr;
    
    // Generate pointer expression
    ValueInfo* ptr_val = codegen_generate_expression(codegen, checker, ptr_deref->pointer);
    if (!ptr_val) {
        codegen_error(codegen, expr->pos, "Failed to generate pointer expression for dereference");
        return NULL;
    }
    
    // Load from the pointer
    LLVMValueRef result = LLVMBuildLoad2(codegen->builder,
                                         LLVMInt8Type(), // Load as i8 for now
                                         ptr_val->llvm_value,
                                         "ptr_deref");
    
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = NULL; // TODO: Determine pointee type
    result_info->is_lvalue = 1; // Dereferenced pointer is an lvalue
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(ptr_val);
    
    return result_info;
#endif
}

ValueInfo* codegen_generate_addr_of(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_ADDR_OF) return NULL;
    
    AddrOfNode* addr_of = (AddrOfNode*)expr;
    
    // Generate the operand expression - must be an lvalue
    ValueInfo* operand_val = codegen_generate_expression(codegen, checker, addr_of->operand);
    if (!operand_val) {
        codegen_error(codegen, expr->pos, "Failed to generate operand expression for address-of");
        return NULL;
    }
    
    if (!operand_val->is_lvalue) {
        value_info_free(operand_val);
        codegen_error(codegen, expr->pos, "Cannot take address of non-lvalue expression");
        return NULL;
    }
    
    // The address is the LLVM value itself (since lvalues store addresses)
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = operand_val->llvm_value;
    result_info->goo_type = NULL; // TODO: Create pointer type
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(operand_val);
    
    return result_info;
#endif
}

ValueInfo* codegen_generate_port_io(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_PORT_IO) return NULL;
    
    PortIONode* port_io = (PortIONode*)expr;
    
    // Generate port number expression
    ValueInfo* port_val = codegen_generate_expression(codegen, checker, port_io->port);
    if (!port_val) {
        codegen_error(codegen, expr->pos, "Failed to generate port number expression");
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    
    if (port_io->is_input) {
        // Port input - generate inline assembly for IN instruction
        LLVMTypeRef port_type = LLVMInt16Type();
        LLVMTypeRef func_type = LLVMFunctionType(LLVMInt8Type(), &port_type, 1, 0);
        const char* asm_str = "inb $1, $0";
        const char* constraints = "=a,Nd,~{dirflag},~{fpsr},~{flags}";
        
        LLVMValueRef inline_asm = LLVMGetInlineAsm(func_type, (char*)asm_str, strlen(asm_str),
                                                   (char*)constraints, strlen(constraints),
                                                   1, 0, LLVMInlineAsmDialectIntel, 0);
        
        result = LLVMBuildCall2(codegen->builder, func_type, inline_asm, &port_val->llvm_value, 1, "port_in");
    } else {
        // Port output - generate inline assembly for OUT instruction
        ValueInfo* value_val = codegen_generate_expression(codegen, checker, port_io->value);
        if (!value_val) {
            value_info_free(port_val);
            codegen_error(codegen, expr->pos, "Failed to generate value expression for port output");
            return NULL;
        }
        
        LLVMTypeRef func_type = LLVMFunctionType(LLVMVoidType(), NULL, 0, 0);
        const char* asm_str = "outb $1, $0";
        const char* constraints = "Nd,a,~{dirflag},~{fpsr},~{flags}";
        
        LLVMValueRef inline_asm = LLVMGetInlineAsm(func_type, (char*)asm_str, strlen(asm_str),
                                                   (char*)constraints, strlen(constraints),
                                                   1, 1, LLVMInlineAsmDialectIntel, 0);
        
        LLVMValueRef args[] = { port_val->llvm_value, value_val->llvm_value };
        result = LLVMBuildCall2(codegen->builder, func_type, inline_asm, args, 2, "port_out");
        
        value_info_free(value_val);
    }
    
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = NULL; // TODO: Set appropriate type
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(port_val);
    
    return result_info;
#endif
}

ValueInfo* codegen_generate_mmio_access(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_MMIO_ACCESS) return NULL;
    
    MMIOAccessNode* mmio = (MMIOAccessNode*)expr;
    
    // Generate address expression
    ValueInfo* addr_val = codegen_generate_expression(codegen, checker, mmio->address);
    if (!addr_val) {
        codegen_error(codegen, expr->pos, "Failed to generate address expression for MMIO");
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    
    if (mmio->value == NULL) {
        // MMIO read
        LLVMTypeRef load_type = (mmio->size == 1) ? LLVMInt8Type() :
                               (mmio->size == 2) ? LLVMInt16Type() :
                               (mmio->size == 4) ? LLVMInt32Type() : LLVMInt64Type();
        
        result = LLVMBuildLoad2(codegen->builder, load_type, addr_val->llvm_value, "mmio_read");
        
        if (mmio->is_volatile) {
            LLVMSetVolatile(result, 1);
        }
    } else {
        // MMIO write
        ValueInfo* value_val = codegen_generate_expression(codegen, checker, mmio->value);
        if (!value_val) {
            value_info_free(addr_val);
            codegen_error(codegen, expr->pos, "Failed to generate value expression for MMIO write");
            return NULL;
        }
        
        result = LLVMBuildStore(codegen->builder, value_val->llvm_value, addr_val->llvm_value);
        
        if (mmio->is_volatile) {
            LLVMSetVolatile(result, 1);
        }
        
        value_info_free(value_val);
    }
    
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = NULL; // TODO: Set appropriate type
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(addr_val);
    
    return result_info;
#endif
}