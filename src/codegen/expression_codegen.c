#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Forward declarations
ValueInfo* codegen_generate_composite_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Expression code generation

ValueInfo* codegen_generate_expression(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    if (!codegen || !checker || !expr) return NULL;
    
    switch (expr->type) {
        case AST_IDENTIFIER:
            return codegen_generate_identifier(codegen, checker, expr);
        case AST_LITERAL:
            return codegen_generate_literal(codegen, checker, expr);
        case AST_COMPOSITE_LIT:
            return codegen_generate_composite_lit(codegen, checker, expr);
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
        // If not found in value table, check if it's a function in the LLVM module
        LLVMValueRef func = LLVMGetNamedFunction(codegen->module, ident->name);
        if (func) {
            // Get function type from type checker
            Variable* func_var = type_checker_lookup_variable(checker, ident->name);
            Type* func_type = NULL;
            if (func_var && func_var->type && func_var->type->kind == TYPE_FUNCTION) {
                func_type = func_var->type;
            }
            // Return function as a value (functions are not lvalues)
            return value_info_new(ident->name, func, func_type);
        }

        codegen_error(codegen, expr->pos, "Undefined identifier '%s'", ident->name);
        return NULL;
    }

    // If it's an lvalue (variable), load the value
    if (value_info->is_lvalue) {
        // Get the value type (not pointer type) for Load2
        LLVMTypeRef value_type = codegen_type_to_llvm(codegen, value_info->goo_type);
        if (!value_type) {
            codegen_error(codegen, expr->pos, "Failed to convert type for identifier '%s'", ident->name);
            return NULL;
        }
        LLVMValueRef loaded_value = LLVMBuildLoad2(codegen->builder, value_type, value_info->llvm_value, ident->name);
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

            // Use type from type checker if available, otherwise default to INT32
            goo_type = expr->node_type ? expr->node_type : type_checker_get_builtin(checker, TYPE_INT32);

            // Generate appropriate LLVM constant based on Goo type
            LLVMTypeRef llvm_type = codegen_type_to_llvm(codegen, goo_type);
            if (!llvm_type) {
                llvm_type = LLVMInt32TypeInContext(codegen->context);
                goo_type = type_checker_get_builtin(checker, TYPE_INT32);
            }

            llvm_value = LLVMConstInt(llvm_type, value, 1);
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
            LLVMValueRef str_global = LLVMAddGlobal(codegen->module,
                LLVMArrayType(LLVMInt8TypeInContext(codegen->context), strlen(literal->value) + 1),
                ".str");
            LLVMSetInitializer(str_global, LLVMConstStringInContext(codegen->context,
                literal->value, strlen(literal->value), 0));
            LLVMSetLinkage(str_global, LLVMPrivateLinkage);
            LLVMSetGlobalConstant(str_global, 1);

            // Get pointer to the string (GEP into the array)
            LLVMValueRef indices[] = {
                LLVMConstInt(LLVMInt64TypeInContext(codegen->context), 0, 0),
                LLVMConstInt(LLVMInt64TypeInContext(codegen->context), 0, 0)
            };
            LLVMValueRef str_ptr = LLVMConstGEP2(
                LLVMArrayType(LLVMInt8TypeInContext(codegen->context), strlen(literal->value) + 1),
                str_global, indices, 2);

            // Create string struct { ptr, len } as a constant
            size_t len = strlen(literal->value);
            LLVMValueRef len_val = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), len, 0);
            LLVMValueRef string_fields[] = {str_ptr, len_val};

            llvm_value = LLVMConstStructInContext(codegen->context, string_fields, 2, 0);
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

ValueInfo* codegen_generate_composite_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_COMPOSITE_LIT) return NULL;

    CompositeLitNode* comp = (CompositeLitNode*)expr;

    // Get the struct type
    Type* struct_type = expr->node_type;
    if (!struct_type || struct_type->kind != TYPE_STRUCT) {
        codegen_error(codegen, expr->pos, "Composite literal must have struct type");
        return NULL;
    }

    // Create LLVM struct type
    LLVMTypeRef llvm_struct_type = codegen_type_to_llvm(codegen, struct_type);

    // Create a zero-initialized struct value
    LLVMValueRef struct_value = LLVMConstNull(llvm_struct_type);

    // If there are field initializers, create a struct with those values
    if (comp->field_count > 0) {
        // Build array of field values (zero for uninitialized fields)
        LLVMValueRef* field_values = (LLVMValueRef*)calloc(struct_type->data.struct_type.field_count,
                                                           sizeof(LLVMValueRef));

        // Initialize all fields to zero first
        for (size_t i = 0; i < struct_type->data.struct_type.field_count; i++) {
            LLVMTypeRef field_type = codegen_type_to_llvm(codegen, struct_type->data.struct_type.fields[i].type);
            field_values[i] = LLVMConstNull(field_type);
        }

        // Set values for explicitly initialized fields
        for (size_t i = 0; i < comp->field_count; i++) {
            const char* field_name = comp->field_names[i];
            ASTNode* field_value_expr = comp->field_values[i];

            // Find field index
            int field_index = -1;
            for (size_t j = 0; j < struct_type->data.struct_type.field_count; j++) {
                if (strcmp(struct_type->data.struct_type.fields[j].name, field_name) == 0) {
                    field_index = (int)j;
                    break;
                }
            }

            if (field_index == -1) {
                codegen_error(codegen, expr->pos, "Unknown field '%s'", field_name);
                free(field_values);
                return NULL;
            }

            // Generate the field value
            ValueInfo* field_val = codegen_generate_expression(codegen, checker, field_value_expr);
            if (!field_val) {
                free(field_values);
                return NULL;
            }

            field_values[field_index] = field_val->llvm_value;
            value_info_free(field_val);
        }

        // Create the struct constant
        struct_value = LLVMConstNamedStruct(llvm_struct_type, field_values,
                                           struct_type->data.struct_type.field_count);
        free(field_values);
    }

    return value_info_new(NULL, struct_value, struct_type);
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
        // Left side must be an lvalue (identifier or index expression)
        ValueInfo* target = NULL;

        if (binary->left->type == AST_IDENTIFIER) {
            // Simple identifier assignment
            IdentifierNode* ident = (IdentifierNode*)binary->left;
            target = codegen_lookup_value(codegen, ident->name);
            if (!target || !target->is_lvalue) {
                codegen_error(codegen, expr->pos, "Invalid assignment target '%s'", ident->name);
                return NULL;
            }
        } else if (binary->left->type == AST_INDEX_EXPR) {
            // Array/slice indexed assignment (e.g., arr[i] = value)
            target = codegen_generate_index_expr(codegen, checker, binary->left);
            if (!target) {
                return NULL;
            }
            if (!target->is_lvalue) {
                codegen_error(codegen, expr->pos, "Index expression is not assignable");
                value_info_free(target);
                return NULL;
            }
        } else if (binary->left->type == AST_SELECTOR_EXPR) {
            // Struct field assignment (e.g., p.age = value)
            target = codegen_generate_selector_expr(codegen, checker, binary->left);
            if (!target) {
                return NULL;
            }
            if (!target->is_lvalue) {
                codegen_error(codegen, expr->pos, "Selector expression is not assignable");
                value_info_free(target);
                return NULL;
            }
        } else {
            codegen_error(codegen, expr->pos, "Assignment target must be an identifier, index expression, or field access");
            return NULL;
        }

        // Generate right side value
        ValueInfo* value = codegen_generate_expression(codegen, checker, binary->right);
        if (!value) {
            if (binary->left->type == AST_INDEX_EXPR || binary->left->type == AST_SELECTOR_EXPR) {
                value_info_free(target);
            }
            return NULL;
        }

        // Store the value
        LLVMBuildStore(codegen->builder, value->llvm_value, target->llvm_value);

        // Free target if it was an index or selector expression (we allocated it)
        if (binary->left->type == AST_INDEX_EXPR || binary->left->type == AST_SELECTOR_EXPR) {
            value_info_free(target);
        }

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
    
    // Get the result type from the AST (already type-checked)
    Type* result_type = expr->node_type;
    if (!result_type) {
        // Fallback: try to infer from operands
        result_type = left_val->goo_type;
    }
    if (!result_type) {
        value_info_free(left_val);
        value_info_free(right_val);
        codegen_error(codegen, expr->pos, "Binary expression has no type information");
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
    
    // Get result type from AST (already type-checked)
    Type* result_type = expr->node_type;
    if (!result_type) {
        // Fallback: use operand type
        result_type = operand->goo_type;
    }
    if (!result_type) {
        value_info_free(operand);
        codegen_error(codegen, expr->pos, "Unary expression has no type information");
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
        if (strcmp(func_name->name, "goo_printf") == 0) {
            return codegen_generate_printf_call(codegen, checker, expr);
        }
        if (strcmp(func_name->name, "make") == 0) {
            // Handle make() built-in for slices, maps, channels
            return codegen_generate_make_call(codegen, checker, expr);
        }
        if (strcmp(func_name->name, "cap") == 0) {
            // Handle cap() built-in - returns capacity of slices/channels
            return codegen_generate_cap_call(codegen, checker, expr);
        }
        if (strcmp(func_name->name, "append") == 0) {
            // Handle append() built-in - appends elements to slice
            return codegen_generate_append_call(codegen, checker, expr);
        }
        if (strcmp(func_name->name, "len") == 0) {
            // Handle len() built-in - returns the length of arrays/slices/strings/maps
            if (!call->args) {
                codegen_error(codegen, expr->pos, "len() requires an argument");
                return NULL;
            }

            // Generate the argument to determine its type
            ValueInfo* arg_val = codegen_generate_expression(codegen, checker, call->args);
            if (!arg_val) return NULL;

            Type* arg_type = arg_val->goo_type;
            LLVMValueRef len_value = NULL;
            Type* int_type = type_checker_get_builtin(checker, TYPE_INT32);

            if (arg_type->kind == TYPE_ARRAY) {
                // For arrays, return the compile-time constant size
                size_t array_length = arg_type->data.array.length;
                len_value = LLVMConstInt(LLVMInt32TypeInContext(codegen->context),
                                        array_length, 0);
                value_info_free(arg_val);
            } else if (arg_type->kind == TYPE_SLICE || arg_type->kind == TYPE_STRING) {
                // For slices/strings, extract the length field (index 1 in struct)
                LLVMValueRef slice_val = arg_val->llvm_value;
                if (arg_val->is_lvalue) {
                    slice_val = LLVMBuildLoad2(codegen->builder,
                                              codegen_type_to_llvm(codegen, arg_type),
                                              slice_val, "slice_load");
                }
                len_value = LLVMBuildExtractValue(codegen->builder, slice_val, 1, "len");
                value_info_free(arg_val);
            } else if (arg_type->kind == TYPE_MAP) {
                // For maps, would need runtime call - TODO
                codegen_error(codegen, expr->pos, "len() for maps not yet implemented");
                value_info_free(arg_val);
                return NULL;
            } else {
                codegen_error(codegen, expr->pos,
                            "len() requires array, slice, string, or map argument");
                value_info_free(arg_val);
                return NULL;
            }

            return value_info_new(NULL, len_value, int_type);
        }
    }

    // Special handling for method calls (obj.method())
    if (call->function->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* selector = (SelectorExprNode*)call->function;

        // Generate the receiver object
        ValueInfo* receiver_val = codegen_generate_expression(codegen, checker, selector->expr);
        if (!receiver_val) return NULL;

        // Get the struct type name (handle both value and pointer receivers)
        Type* receiver_type = receiver_val->goo_type;
        const char* type_name = NULL;

        if (receiver_type->kind == TYPE_STRUCT) {
            type_name = receiver_type->data.struct_type.name;
        } else if (receiver_type->kind == TYPE_POINTER &&
                   receiver_type->data.pointer.pointee_type &&
                   receiver_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
            type_name = receiver_type->data.pointer.pointee_type->data.struct_type.name;
        }

        if (type_name) {
            // Build mangled method name: TypeName_methodName
            size_t len = strlen(type_name) + strlen(selector->selector) + 2;
            char* mangled_name = malloc(len);
            snprintf(mangled_name, len, "%s_%s", type_name, selector->selector);

            // Look up the method in LLVM module
            LLVMValueRef method_func = LLVMGetNamedFunction(codegen->module, mangled_name);
            free(mangled_name);

            if (method_func) {
                // It's a method call! Generate call with receiver as first argument
                // Count arguments: receiver + actual arguments
                size_t arg_count = 1;  // Start with receiver
                ASTNode* arg = call->args;
                while (arg) {
                    arg_count++;
                    arg = arg->next;
                }

                LLVMValueRef* args = malloc(sizeof(LLVMValueRef) * arg_count);
                if (!args) {
                    value_info_free(receiver_val);
                    return NULL;
                }

                // First argument is the receiver
                // Load if it's an lvalue
                if (receiver_val->is_lvalue) {
                    args[0] = LLVMBuildLoad2(codegen->builder,
                                            codegen_type_to_llvm(codegen, receiver_type),
                                            receiver_val->llvm_value,
                                            "receiver_load");
                } else {
                    args[0] = receiver_val->llvm_value;
                }
                value_info_free(receiver_val);

                // Add actual arguments
                arg = call->args;
                for (size_t i = 1; i < arg_count; i++) {
                    ValueInfo* arg_val = codegen_generate_expression(codegen, checker, arg);
                    if (!arg_val) {
                        free(args);
                        return NULL;
                    }
                    args[i] = arg_val->llvm_value;
                    value_info_free(arg_val);
                    arg = arg->next;
                }

                // Generate the method call
                LLVMTypeRef func_type = LLVMGlobalGetValueType(method_func);
                LLVMValueRef result = LLVMBuildCall2(codegen->builder, func_type,
                                                    method_func, args,
                                                    (unsigned)arg_count, "method_call");
                free(args);

                // Get return type from AST (type checker already validated this)
                Type* return_type = expr->node_type;

                if (!return_type) {
                    codegen_error(codegen, expr->pos, "Method call has no type information");
                    return NULL;
                }

                return value_info_new(NULL, result, return_type);
            }
        }

        // Not a method, fall through to regular selector handling
        value_info_free(receiver_val);
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
    // Get the function type - in LLVM with opaque pointers, use LLVMGlobalGetValueType for functions
    LLVMTypeRef func_type = LLVMGlobalGetValueType(func_val->llvm_value);
    LLVMValueRef result = LLVMBuildCall2(codegen->builder, func_type, func_val->llvm_value, args, (unsigned)arg_count, "call");
    
    free(args);

    // Get return type from AST (already type-checked)
    Type* return_type = expr->node_type;
    if (!return_type && func_val->goo_type && func_val->goo_type->kind == TYPE_FUNCTION) {
        // Fallback: use function's return type
        return_type = func_val->goo_type->data.function.return_type;
    }

    value_info_free(func_val);

    if (!return_type) {
        codegen_error(codegen, expr->pos, "Call expression has no type information");
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
    // Special case: if base is an identifier, look it up directly to get the lvalue pointer
    ValueInfo* base_val = NULL;
    if (selector->expr->type == AST_IDENTIFIER) {
        IdentifierNode* ident = (IdentifierNode*)selector->expr;
        base_val = codegen_lookup_value(codegen, ident->name);
        if (!base_val) {
            codegen_error(codegen, expr->pos, "Undefined identifier '%s'", ident->name);
            return NULL;
        }
        // Don't free this - it's from the symbol table
        // Make a copy so we can free it later
        base_val = value_info_new(ident->name, base_val->llvm_value, base_val->goo_type);
        base_val->is_lvalue = 1;  // Variables are always lvalues
    } else {
        base_val = codegen_generate_expression(codegen, checker, selector->expr);
        if (!base_val) {
            codegen_error(codegen, expr->pos, "Failed to generate base expression for selector");
            return NULL;
        }
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

// Special handling for goo_printf calls to convert Goo strings to C strings
ValueInfo* codegen_generate_printf_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    
    CallExprNode* call = (CallExprNode*)expr;
    
    // Get the goo_printf function
    LLVMValueRef printf_func = LLVMGetNamedFunction(codegen->module, "goo_printf");
    if (!printf_func) {
        codegen_error(codegen, expr->pos, "goo_printf function not found");
        return NULL;
    }
    
    // Count arguments
    size_t arg_count = 0;
    ASTNode* arg = call->args;
    while (arg) {
        arg_count++;
        arg = arg->next;
    }
    
    if (arg_count == 0) {
        codegen_error(codegen, expr->pos, "goo_printf requires at least one argument");
        return NULL;
    }
    
    // Generate arguments and convert string structs to C strings
    LLVMValueRef* args = malloc(sizeof(LLVMValueRef) * arg_count);
    if (!args) return NULL;
    
    arg = call->args;
    for (size_t i = 0; i < arg_count; i++) {
        ValueInfo* arg_val = codegen_generate_expression(codegen, checker, arg);
        if (!arg_val) {
            for (size_t j = 0; j < i; j++) {
                // Cleanup previous args if needed
            }
            free(args);
            return NULL;
        }
        
        // Convert Goo string structs to C char* for printf
        if (arg_val->goo_type && arg_val->goo_type->kind == TYPE_STRING) {
            // Extract the char* from the string struct { ptr, len }
            args[i] = LLVMBuildExtractValue(codegen->builder, arg_val->llvm_value, 0, "string_ptr");
        } else {
            args[i] = arg_val->llvm_value;
        }
        
        value_info_free(arg_val);
        arg = arg->next;
    }
    
    // Call goo_printf
    LLVMValueRef result = LLVMBuildCall2(codegen->builder, 
                                        LLVMGlobalGetValueType(printf_func),
                                        printf_func, args, arg_count, "");
    
    free(args);
    
    // Return void value info
    ValueInfo* result_info = value_info_new(NULL, result, type_checker_get_builtin(checker, TYPE_VOID));
    return result_info;
#endif
}

// make() builtin for slices, maps, channels
ValueInfo* codegen_generate_make_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;
    if (!call->args) {
        codegen_error(codegen, expr->pos, "make() requires a type argument");
        return NULL;
    }

    // First argument should be a type
    ASTNode* type_arg = call->args;
    Type* slice_type = NULL;

    // Determine the slice type from the type argument
    if (type_arg->type == AST_SLICE_TYPE) {
        slice_type = type_from_ast(checker, type_arg);
        if (!slice_type) {
            codegen_error(codegen, expr->pos, "Failed to resolve slice type");
            return NULL;
        }
    } else {
        codegen_error(codegen, expr->pos, "make() currently only supports slice types");
        return NULL;
    }

    // Get length argument (required)
    ASTNode* len_arg = type_arg->next;
    if (!len_arg) {
        codegen_error(codegen, expr->pos, "make() for slices requires length argument");
        return NULL;
    }

    ValueInfo* len_val = codegen_generate_expression(codegen, checker, len_arg);
    if (!len_val) return NULL;

    // Get capacity argument (optional, defaults to length)
    LLVMValueRef cap_llvm;
    ASTNode* cap_arg = len_arg->next;
    if (cap_arg) {
        ValueInfo* cap_val = codegen_generate_expression(codegen, checker, cap_arg);
        if (!cap_val) {
            value_info_free(len_val);
            return NULL;
        }
        cap_llvm = cap_val->llvm_value;
        value_info_free(cap_val);
    } else {
        cap_llvm = len_val->llvm_value;
    }

    // Get LLVM types
    LLVMTypeRef slice_llvm_type = codegen_type_to_llvm(codegen, slice_type);
    LLVMTypeRef element_llvm_type = codegen_type_to_llvm(codegen, slice_type->data.slice.element_type);

    // Allocate array: ptr = malloc(cap * sizeof(element))
    LLVMValueRef element_size = LLVMSizeOf(element_llvm_type);
    LLVMValueRef alloc_size = LLVMBuildMul(codegen->builder, cap_llvm, element_size, "alloc_size");

    // Call malloc
    LLVMTypeRef malloc_type = LLVMFunctionType(LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                                                (LLVMTypeRef[]){LLVMInt64TypeInContext(codegen->context)}, 1, 0);
    LLVMValueRef malloc_func = LLVMGetNamedFunction(codegen->module, "malloc");
    if (!malloc_func) {
        malloc_func = LLVMAddFunction(codegen->module, "malloc", malloc_type);
    }

    LLVMValueRef alloc_size_64 = LLVMBuildZExtOrBitCast(codegen->builder, alloc_size,
                                                         LLVMInt64TypeInContext(codegen->context), "alloc_size_64");
    LLVMValueRef ptr = LLVMBuildCall2(codegen->builder, malloc_type, malloc_func,
                                     (LLVMValueRef[]){alloc_size_64}, 1, "slice_data");

    // Cast to element pointer type
    ptr = LLVMBuildBitCast(codegen->builder, ptr, LLVMPointerType(element_llvm_type, 0), "slice_ptr");

    // Build slice struct: {ptr, len, cap}
    LLVMValueRef slice_val = LLVMGetUndef(slice_llvm_type);
    slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, ptr, 0, "slice.ptr");
    slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, len_val->llvm_value, 1, "slice.len");
    slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, cap_llvm, 2, "slice.cap");

    value_info_free(len_val);

    ValueInfo* result = value_info_new(NULL, slice_val, slice_type);
    return result;
#endif
}

// cap() builtin for slices and channels
ValueInfo* codegen_generate_cap_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;
    if (!call->args) {
        codegen_error(codegen, expr->pos, "cap() requires an argument");
        return NULL;
    }

    // Generate the argument
    ValueInfo* arg_val = codegen_generate_expression(codegen, checker, call->args);
    if (!arg_val) return NULL;

    Type* arg_type = arg_val->goo_type;
    LLVMValueRef cap_value = NULL;
    Type* int_type = type_checker_get_builtin(checker, TYPE_INT32);

    if (arg_type->kind == TYPE_SLICE) {
        // For slices, extract the capacity field (index 2 in struct)
        LLVMValueRef slice_val = arg_val->llvm_value;
        if (arg_val->is_lvalue) {
            slice_val = LLVMBuildLoad2(codegen->builder,
                                      codegen_type_to_llvm(codegen, arg_type),
                                      slice_val, "slice_load");
        }
        cap_value = LLVMBuildExtractValue(codegen->builder, slice_val, 2, "cap");
        value_info_free(arg_val);
    } else if (arg_type->kind == TYPE_CHANNEL) {
        // For channels, would need runtime call - TODO
        codegen_error(codegen, expr->pos, "cap() for channels not yet implemented");
        value_info_free(arg_val);
        return NULL;
    } else {
        codegen_error(codegen, expr->pos, "cap() requires slice or channel argument");
        value_info_free(arg_val);
        return NULL;
    }

    ValueInfo* result = value_info_new(NULL, cap_value, int_type);
    return result;
#endif
}

// append() builtin for slices
ValueInfo* codegen_generate_append_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;
    if (!call->args) {
        codegen_error(codegen, expr->pos, "append() requires at least one argument");
        return NULL;
    }

    // First argument is the slice
    ValueInfo* slice_val = codegen_generate_expression(codegen, checker, call->args);
    if (!slice_val) return NULL;

    Type* slice_type = slice_val->goo_type;
    if (slice_type->kind != TYPE_SLICE) {
        codegen_error(codegen, expr->pos, "First argument to append() must be a slice");
        value_info_free(slice_val);
        return NULL;
    }

    // For now, implement simple append of one element
    // TODO: Handle multiple elements and slice append
    ASTNode* elem_arg = call->args->next;
    if (!elem_arg) {
        // No elements to append, return original slice
        return slice_val;
    }

    ValueInfo* elem_val = codegen_generate_expression(codegen, checker, elem_arg);
    if (!elem_val) {
        value_info_free(slice_val);
        return NULL;
    }

    // Extract slice fields
    LLVMValueRef slice = slice_val->llvm_value;
    if (slice_val->is_lvalue) {
        slice = LLVMBuildLoad2(codegen->builder,
                              codegen_type_to_llvm(codegen, slice_type),
                              slice, "slice_load");
    }

    LLVMValueRef ptr = LLVMBuildExtractValue(codegen->builder, slice, 0, "ptr");
    LLVMValueRef len = LLVMBuildExtractValue(codegen->builder, slice, 1, "len");
    LLVMValueRef cap = LLVMBuildExtractValue(codegen->builder, slice, 2, "cap");

    // Check if we need to grow: len < cap
    LLVMValueRef need_grow = LLVMBuildICmp(codegen->builder, LLVMIntEQ, len, cap, "need_grow");

    LLVMBasicBlockRef grow_block = codegen_create_block(codegen, "grow");
    LLVMBasicBlockRef no_grow_block = codegen_create_block(codegen, "no_grow");
    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "merge");

    LLVMBuildCondBr(codegen->builder, need_grow, grow_block, no_grow_block);

    // Grow block: allocate new array with double capacity
    codegen_set_insert_point(codegen, grow_block);
    LLVMValueRef new_cap = LLVMBuildMul(codegen->builder, cap,
                                        LLVMConstInt(LLVMTypeOf(cap), 2, 0), "new_cap");
    // Handle zero capacity
    LLVMValueRef cap_is_zero = LLVMBuildICmp(codegen->builder, LLVMIntEQ, cap,
                                             LLVMConstInt(LLVMTypeOf(cap), 0, 0), "cap_is_zero");
    new_cap = LLVMBuildSelect(codegen->builder, cap_is_zero,
                              LLVMConstInt(LLVMTypeOf(cap), 1, 0), new_cap, "new_cap_fixed");

    LLVMTypeRef element_type = codegen_type_to_llvm(codegen, slice_type->data.slice.element_type);
    LLVMValueRef element_size = LLVMSizeOf(element_type);
    LLVMValueRef new_alloc_size = LLVMBuildMul(codegen->builder, new_cap, element_size, "new_alloc_size");

    // malloc
    LLVMTypeRef malloc_type = LLVMFunctionType(LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                                                (LLVMTypeRef[]){LLVMInt64TypeInContext(codegen->context)}, 1, 0);
    LLVMValueRef malloc_func = LLVMGetNamedFunction(codegen->module, "malloc");
    if (!malloc_func) {
        malloc_func = LLVMAddFunction(codegen->module, "malloc", malloc_type);
    }

    LLVMValueRef new_alloc_size_64 = LLVMBuildZExtOrBitCast(codegen->builder, new_alloc_size,
                                                             LLVMInt64TypeInContext(codegen->context), "new_alloc_size_64");
    LLVMValueRef new_ptr = LLVMBuildCall2(codegen->builder, malloc_type, malloc_func,
                                          (LLVMValueRef[]){new_alloc_size_64}, 1, "new_ptr");
    new_ptr = LLVMBuildBitCast(codegen->builder, new_ptr, LLVMTypeOf(ptr), "new_ptr_cast");

    // TODO: memcpy old data to new array
    LLVMValueRef ptr_grow = new_ptr;
    LLVMValueRef cap_grow = new_cap;
    LLVMBuildBr(codegen->builder, merge_block);

    // No grow block: use existing array
    codegen_set_insert_point(codegen, no_grow_block);
    LLVMBuildBr(codegen->builder, merge_block);

    // Merge block: phi for ptr and cap
    codegen_set_insert_point(codegen, merge_block);
    LLVMValueRef ptr_phi = LLVMBuildPhi(codegen->builder, LLVMTypeOf(ptr), "ptr_phi");
    LLVMValueRef cap_phi = LLVMBuildPhi(codegen->builder, LLVMTypeOf(cap), "cap_phi");

    LLVMValueRef ptr_values[] = {ptr_grow, ptr};
    LLVMBasicBlockRef ptr_blocks[] = {grow_block, no_grow_block};
    LLVMAddIncoming(ptr_phi, ptr_values, ptr_blocks, 2);

    LLVMValueRef cap_values[] = {cap_grow, cap};
    LLVMBasicBlockRef cap_blocks[] = {grow_block, no_grow_block};
    LLVMAddIncoming(cap_phi, cap_values, cap_blocks, 2);

    // Store element at ptr[len]
    LLVMValueRef elem_ptr = LLVMBuildGEP2(codegen->builder, element_type, ptr_phi,
                                          (LLVMValueRef[]){len}, 1, "elem_ptr");
    LLVMBuildStore(codegen->builder, elem_val->llvm_value, elem_ptr);

    // Increment len
    LLVMValueRef new_len = LLVMBuildAdd(codegen->builder, len,
                                        LLVMConstInt(LLVMTypeOf(len), 1, 0), "new_len");

    // Build new slice
    LLVMTypeRef slice_llvm_type = codegen_type_to_llvm(codegen, slice_type);
    LLVMValueRef new_slice = LLVMGetUndef(slice_llvm_type);
    new_slice = LLVMBuildInsertValue(codegen->builder, new_slice, ptr_phi, 0, "new_slice.ptr");
    new_slice = LLVMBuildInsertValue(codegen->builder, new_slice, new_len, 1, "new_slice.len");
    new_slice = LLVMBuildInsertValue(codegen->builder, new_slice, cap_phi, 2, "new_slice.cap");

    value_info_free(slice_val);
    value_info_free(elem_val);

    ValueInfo* result = value_info_new(NULL, new_slice, slice_type);
    return result;
#endif
}